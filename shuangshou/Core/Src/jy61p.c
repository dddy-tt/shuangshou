#include "jy61p.h"
#include "i2c.h"
#include "string.h"

#define JY61P_REG_ACC_START     JY61P_REG_AX_L
#define JY61P_REG_GYRO_START    JY61P_REG_GX_L
#define JY61P_REG_ANGLE_START   JY61P_REG_ROLL_L
#define JY61P_VECTOR_LEN        6U

JY61P_Data_t JY61P_Right;
JY61P_Data_t JY61P_Left;

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

static uint8_t jy61p_read_vector(I2C_HandleTypeDef *hi2c, uint8_t reg_start, int16_t out_raw[3])
{
    uint8_t buf[JY61P_VECTOR_LEN];
    HAL_StatusTypeDef st;

    st = HAL_I2C_Mem_Read(hi2c, JY61P_ADDR_HAL, reg_start,
                          I2C_MEMADD_SIZE_8BIT, buf, JY61P_VECTOR_LEN, 100);
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

uint8_t JY61P_Init(uint8_t channel)
{
    I2C_HandleTypeDef *hi2c = channel_to_hi2c(channel);
    JY61P_Data_t *data = data_of(channel);

    memset(data, 0, sizeof(JY61P_Data_t));

    /* Probe with retry: JY61P may not be ready right after power-on. */
    {
        uint8_t probe_buf[2];
        HAL_StatusTypeDef pst;
        uint8_t try = 0;
        do {
            pst = HAL_I2C_Mem_Read(hi2c, JY61P_ADDR_HAL, JY61P_REG_ROLL_L,
                                   I2C_MEMADD_SIZE_8BIT, probe_buf, 2, 100);
            if (pst == HAL_OK) break;
            HAL_Delay(100);
        } while (++try < 5);
        if (pst != HAL_OK) {
            data->online = 0U;
            return 1U;
        }
    }

    /* Read initial angle — skip all-zero check because JY61P may still be
     * settling after power-on.  The 200Hz runtime task will pick up live
     * data regardless. */
    (void)jy61p_read_vector(hi2c, JY61P_REG_ANGLE_START, data->angle_raw);

    jy61p_publish_angle(data, 0);
    data->online = 1U;
    return 0U;
}

uint8_t JY61P_Read_Data(uint8_t channel, float *acc, float *gyro, float *angle)
{
    I2C_HandleTypeDef *hi2c = channel_to_hi2c(channel);
    JY61P_Data_t *data = data_of(channel);
    uint8_t err = 0U;

    if (jy61p_read_vector(hi2c, JY61P_REG_ACC_START, data->acc_raw) != 0U) {
        err |= 1U;
    } else {
        jy61p_publish_acc(data, acc);
    }

    if (jy61p_read_vector(hi2c, JY61P_REG_GYRO_START, data->gyro_raw) != 0U) {
        err |= 2U;
    } else {
        jy61p_publish_gyro(data, gyro);
    }

    if (jy61p_read_vector(hi2c, JY61P_REG_ANGLE_START, data->angle_raw) != 0U) {
        err |= 4U;
    } else {
        jy61p_publish_angle(data, angle);
    }

    data->online = (err == 0U) ? 1U : 0U;
    return err;
}

uint8_t JY61P_Read_Angle(uint8_t channel, float *roll, float *pitch, float *yaw)
{
    I2C_HandleTypeDef *hi2c = channel_to_hi2c(channel);
    JY61P_Data_t *data = data_of(channel);

    if (jy61p_read_vector(hi2c, JY61P_REG_ANGLE_START, data->angle_raw) != 0U) {
        data->online = 0U;
        return 1U;
    }

    jy61p_publish_angle(data, 0);
    data->online = 1U;

    if (roll != 0)  *roll = data->angle[0];
    if (pitch != 0) *pitch = data->angle[1];
    if (yaw != 0)   *yaw = data->angle[2];

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
