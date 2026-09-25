#include "jy61p.h"
#include "i2c.h"
#include "string.h"

#define JY61P_REG_ACC_START     JY61P_REG_AX_L
#define JY61P_REG_GYRO_START    JY61P_REG_GX_L
#define JY61P_REG_ANGLE_START   JY61P_REG_ROLL_L
#define JY61P_VECTOR_LEN        6U
#define JY61P_I2C_TIMEOUT_MS       10U

JY61P_Data_t JY61P_Right;
JY61P_Data_t JY61P_Left;

enum {
    JY61P_DIAG_ACC = 0U,
    JY61P_DIAG_GYRO,
    JY61P_DIAG_ANGLE,
    JY61P_DIAG_PROBE,
    JY61P_DIAG_COUNT
};

static uint32_t jy61p_last_hal_error[2][JY61P_DIAG_COUNT];

/* 与主调度器共用 TIM3 的毫秒时间基准，便于判断样本是否过期。 */
extern volatile uint32_t sys_tick_ms;

/* 8-bit I2C address for HAL (HAL expects address << 1 format) */
#define JY61P_ADDR_HAL          JY61P_I2C_ADDR  /* 0xA0 */

static I2C_HandleTypeDef *channel_to_hi2c(uint8_t channel)
{
    return (channel == JY61P_CH_RIGHT) ? &hi2c1 : &hi2c2;
}

static JY61P_Data_t *data_of(uint8_t channel)
{
    return (channel == JY61P_CH_RIGHT) ? &JY61P_Right : &JY61P_Left;
}

static uint8_t jy61p_channel_index(uint8_t channel)
{
    return (channel == JY61P_CH_RIGHT) ? 0U : 1U;
}

static void jy61p_record_result(JY61P_Data_t *data, uint8_t error_mask)
{
    data->last_error = error_mask;
    if (error_mask == 0U || error_mask == JY61P_ERR_ANGLE_ZERO) {
        /* 六个姿态字节全零是已应答的异常快照，不是总线离线。 */
        data->online = 1U;
        data->error_streak = 0U;
        return;
    }

    if (data->error_streak < 0xFFU) {
        data->error_streak++;
    }
    /* 保留少量瞬态容错，但连续失败后必须反映为当前离线状态。 */
    if (data->error_streak >= 3U) {
        data->online = 0U;
    }
}

static int16_t bytes_to_s16(uint8_t low, uint8_t high)
{
    return (int16_t)(((uint16_t)high << 8) | (uint16_t)low);
}

static float jy61p_scale_acc(int16_t raw)
{
    return ((float)raw / 32768.0f) * 16.0f;
}

static float jy61p_scale_gyro(int16_t raw)
{
    return ((float)raw / 32768.0f) * 2000.0f;
}

static float jy61p_scale_angle(int16_t raw)
{
    return ((float)raw / 32768.0f) * 180.0f;
}

static uint8_t jy61p_read_vector(uint8_t channel, I2C_HandleTypeDef *hi2c,
                                 uint8_t reg_start, uint8_t diag_index,
                                 int16_t out_raw[3])
{
    uint8_t buf[JY61P_VECTOR_LEN];
    HAL_StatusTypeDef st;

    st = HAL_I2C_Mem_Read(hi2c, JY61P_ADDR_HAL, reg_start,
                          I2C_MEMADD_SIZE_8BIT, buf, JY61P_VECTOR_LEN,
                          JY61P_I2C_TIMEOUT_MS);
    jy61p_last_hal_error[jy61p_channel_index(channel)][diag_index] =
        (st == HAL_OK) ? HAL_I2C_ERROR_NONE : HAL_I2C_GetError(hi2c);
    if (st != HAL_OK) {
        return 1U;
    }

    out_raw[0] = bytes_to_s16(buf[0], buf[1]);
    out_raw[1] = bytes_to_s16(buf[2], buf[3]);
    out_raw[2] = bytes_to_s16(buf[4], buf[5]);
    return 0U;
}

static void jy61p_publish_acc(JY61P_Data_t *data, float *acc)
{
    data->acc[0] = jy61p_scale_acc(data->acc_raw[0]);
    data->acc[1] = jy61p_scale_acc(data->acc_raw[1]);
    data->acc[2] = jy61p_scale_acc(data->acc_raw[2]);

    if (acc != 0) {
        acc[0] = data->acc[0];
        acc[1] = data->acc[1];
        acc[2] = data->acc[2];
    }
}

static void jy61p_publish_gyro(JY61P_Data_t *data, float *gyro)
{
    data->gyro[0] = jy61p_scale_gyro(data->gyro_raw[0]);
    data->gyro[1] = jy61p_scale_gyro(data->gyro_raw[1]);
    data->gyro[2] = jy61p_scale_gyro(data->gyro_raw[2]);

    if (gyro != 0) {
        gyro[0] = data->gyro[0];
        gyro[1] = data->gyro[1];
        gyro[2] = data->gyro[2];
    }
}

static void jy61p_publish_angle(JY61P_Data_t *data, float *angle)
{
    data->angle[0] = jy61p_scale_angle(data->angle_raw[0]);
    data->angle[1] = jy61p_scale_angle(data->angle_raw[1]);
    data->angle[2] = jy61p_scale_angle(data->angle_raw[2]);

    if (angle != 0) {
        angle[0] = data->angle[0];
        angle[1] = data->angle[1];
        angle[2] = data->angle[2];
    }
}

static uint8_t jy61p_angle_raw_is_zero(const int16_t raw[3])
{
    return (raw[0] == 0 && raw[1] == 0 && raw[2] == 0) ? 1U : 0U;
}

/*
 * JY61P occasionally acknowledges an I2C read while returning six zero
 * bytes.  A real sensor can cross one or two zero axes, but all three raw
 * 16-bit angles being exactly zero is treated as an invalid transport
 * snapshot.  Keep the last good pose until a non-zero snapshot arrives.
 */
static uint8_t jy61p_accept_angle_raw(JY61P_Data_t *data,
                                      const int16_t raw[3], float *angle)
{
    if (jy61p_angle_raw_is_zero(raw) != 0U) {
        if (data->angle_zero_streak < 0xFFU) {
            data->angle_zero_streak++;
        }
        return 0U;
    }

    data->angle_zero_streak = 0U;
    memcpy(data->angle_raw, raw, sizeof(data->angle_raw));
    jy61p_publish_angle(data, angle);
    data->angle_sample_seen = 1U;
    return 1U;
}

uint8_t JY61P_Init(uint8_t channel)
{
    I2C_HandleTypeDef *hi2c = channel_to_hi2c(channel);
    JY61P_Data_t *data = data_of(channel);
    int16_t angle_raw[3];

    memset(data, 0, sizeof(JY61P_Data_t));
    memset(jy61p_last_hal_error[jy61p_channel_index(channel)], 0,
           sizeof(jy61p_last_hal_error[0]));

    /* Probe with retry: JY61P may not be ready right after power-on. */
    {
        uint8_t probe_buf[2];
        HAL_StatusTypeDef pst;
        uint8_t try = 0;
        do {
            pst = HAL_I2C_Mem_Read(hi2c, JY61P_ADDR_HAL, JY61P_REG_ROLL_L,
                                   I2C_MEMADD_SIZE_8BIT, probe_buf, 2,
                                   JY61P_I2C_TIMEOUT_MS);
            jy61p_last_hal_error[jy61p_channel_index(channel)][JY61P_DIAG_PROBE] =
                (pst == HAL_OK) ? HAL_I2C_ERROR_NONE : HAL_I2C_GetError(hi2c);
            if (pst == HAL_OK) break;
            HAL_Delay(100);
        } while (++try < 5);
        if (pst != HAL_OK) {
            data->online = 0U;
            data->error_streak = 0U;
            data->last_error = JY61P_ERR_PROBE;
            return 1U;
        }
    }

    /* 全零姿态帧不作为当前有效样本；保存最后一个非零有效姿态。 */
    if (jy61p_read_vector(channel, hi2c, JY61P_REG_ANGLE_START,
                          JY61P_DIAG_ANGLE, angle_raw) == 0U) {
        if (jy61p_accept_angle_raw(data, angle_raw, 0) != 0U) {
            data->angle_valid = 1U;
            data->angle_updated_ms = sys_tick_ms;
            data->last_error = 0U;
            data->error_streak = 0U;
        } else {
            data->angle_valid = 0U;
            data->last_error = JY61P_ERR_ANGLE_ZERO;
            data->error_streak = 0U;
        }
    } else {
        data->last_error = JY61P_ERR_ANGLE;
        data->error_streak = 1U;
    }
    data->online = 1U;
    return 0U;
}

uint8_t JY61P_Read_Data(uint8_t channel, float *acc, float *gyro, float *angle)
{
    I2C_HandleTypeDef *hi2c = channel_to_hi2c(channel);
    JY61P_Data_t *data = data_of(channel);
    uint8_t err = 0U;
    uint32_t read_tick;
    int16_t angle_raw[3];

    if (jy61p_read_vector(channel, hi2c, JY61P_REG_ACC_START,
                          JY61P_DIAG_ACC, data->acc_raw) != 0U) {
        err |= JY61P_ERR_ACC;
        data->acc_valid = 0U;
    } else {
        jy61p_publish_acc(data, acc);
        data->acc_valid = 1U;
        data->acc_sample_seen = 1U;
        data->acc_updated_ms = sys_tick_ms;
    }

    if (jy61p_read_vector(channel, hi2c, JY61P_REG_GYRO_START,
                          JY61P_DIAG_GYRO, data->gyro_raw) != 0U) {
        err |= JY61P_ERR_GYRO;
    } else {
        jy61p_publish_gyro(data, gyro);
    }

    if (jy61p_read_vector(channel, hi2c, JY61P_REG_ANGLE_START,
                          JY61P_DIAG_ANGLE, angle_raw) != 0U) {
        err |= JY61P_ERR_ANGLE;
        data->angle_valid = 0U;
        data->angle_zero_streak = 0U;
    } else if (jy61p_accept_angle_raw(data, angle_raw, angle) == 0U) {
        err |= JY61P_ERR_ANGLE_ZERO;
        data->angle_valid = 0U;
    } else {
        data->angle_valid = 1U;
    }

    /* 三组 I2C 事务可能跨越多个 tick；只在整次读取结束时取一次时间，
       让同一快照的 ACC/姿态时间戳一致，主循环不会用旧 now 错判新鲜度。 */
    read_tick = sys_tick_ms;
    if (data->acc_valid != 0U) data->acc_updated_ms = read_tick;
    if (data->angle_valid != 0U) data->angle_updated_ms = read_tick;

    /* Publish the complete transaction result for the runtime diagnostic. */
    jy61p_record_result(data, err);
    return err;
}

uint8_t JY61P_Read_Angle(uint8_t channel, float *roll, float *pitch, float *yaw)
{
    I2C_HandleTypeDef *hi2c = channel_to_hi2c(channel);
    JY61P_Data_t *data = data_of(channel);
    uint32_t read_tick;
    int16_t angle_raw[3];

    if (jy61p_read_vector(channel, hi2c, JY61P_REG_ANGLE_START,
                          JY61P_DIAG_ANGLE, angle_raw) != 0U) {
        data->angle_valid = 0U;
        data->angle_zero_streak = 0U;
        jy61p_record_result(data, JY61P_ERR_ANGLE);
        return 1U;
    }

    if (jy61p_accept_angle_raw(data, angle_raw, 0) == 0U) {
        data->angle_valid = 0U;
        jy61p_record_result(data, JY61P_ERR_ANGLE_ZERO);
        return 1U;
    }

    data->angle_valid = 1U;
    read_tick = sys_tick_ms;
    data->angle_updated_ms = read_tick;
    jy61p_record_result(data, 0U);

    if (roll != 0)  *roll = data->angle[0];
    if (pitch != 0) *pitch = data->angle[1];
    if (yaw != 0)   *yaw = data->angle[2];

    return 0U;
}

uint8_t JY61P_TryRecover(uint8_t channel)
{
    I2C_HandleTypeDef *hi2c = channel_to_hi2c(channel);
    JY61P_Data_t *data = data_of(channel);
    uint8_t probe[2];

    /* A short probe is deliberate: a disconnected module must not block the
     * 5-ms scheduler for the 100-ms timeout used by normal transfers.  No
     * sensor reset register or undocumented bus-pulse command is sent. */
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(
        hi2c, JY61P_ADDR_HAL, JY61P_REG_ROLL_L,
        I2C_MEMADD_SIZE_8BIT, probe, sizeof(probe), 5U);
    jy61p_last_hal_error[jy61p_channel_index(channel)][JY61P_DIAG_PROBE] =
        (status == HAL_OK) ? HAL_I2C_ERROR_NONE : HAL_I2C_GetError(hi2c);
    if (status != HAL_OK) {
        /* A transient bus error can leave the HAL I2C state machine unusable
         * even after the sensor is electrically fine again.  Rebuild only
         * this controller, then probe once more with the short timeout. */
        (void)HAL_I2C_DeInit(hi2c);
        if (channel == JY61P_CH_RIGHT) {
            MX_I2C1_Init();
        } else {
            MX_I2C2_Init();
        }
        status = HAL_I2C_Mem_Read(hi2c, JY61P_ADDR_HAL, JY61P_REG_ROLL_L,
                                  I2C_MEMADD_SIZE_8BIT, probe, sizeof(probe), 5U);
        jy61p_last_hal_error[jy61p_channel_index(channel)][JY61P_DIAG_PROBE] =
            (status == HAL_OK) ? HAL_I2C_ERROR_NONE : HAL_I2C_GetError(hi2c);
        if (status != HAL_OK) {
            jy61p_record_result(data, JY61P_ERR_PROBE);
            return 1U;
        }
    }

    jy61p_record_result(data, 0U);
    data->acc_valid = 0U;
    data->angle_valid = 0U;
    data->acc_sample_seen = 0U;
    data->angle_sample_seen = 0U;
    data->angle_zero_streak = 0U;
    data->acc_updated_ms = 0U;
    data->angle_updated_ms = 0U;
    return 0U;
}

uint8_t JY61P_IsOnline(uint8_t channel)
{
    return data_of(channel)->online;
}

void JY61P_GetLastAngle(uint8_t channel, float *roll, float *pitch, float *yaw)
{
    JY61P_Data_t *data = data_of(channel);

    if (roll != 0)  *roll = data->angle[0];
    if (pitch != 0) *pitch = data->angle[1];
    if (yaw != 0)   *yaw = data->angle[2];
}

void JY61P_GetLastHalErrors(uint8_t channel, uint32_t errors[4])
{
    uint8_t i;
    if (errors == 0) return;
    for (i = 0U; i < JY61P_DIAG_COUNT; i++) {
        errors[i] = jy61p_last_hal_error[jy61p_channel_index(channel)][i];
    }
}
