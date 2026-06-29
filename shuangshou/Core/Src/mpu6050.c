#include "mpu6050.h"
#include "soft_i2c.h"
#include "math.h"
#include "string.h"
#include "stm32f4xx_hal.h"

MPU6050_t MPU6050_Right;
MPU6050_t MPU6050_Left;

/* ── 互补滤波系数 ── */
#define ALPHA 0.98f  /* 信任陀螺仪 98%，加速度计 2% */

/* ── 陀螺仪校准的采样次数 ── */
#define CALIB_SAMPLES 200

/* ── 内部工具函数 ── */

/* 将两个 8 位寄存器拼成 16 位有符号数 */
static int16_t i2c2s16(uint8_t hi, uint8_t lo)
{
    return (int16_t)(((uint16_t)hi << 8) | lo);
}

/* 读取 14 字节批量数据的内部实现 */
static uint8_t read_all_raw(uint8_t hand /* 0=右手, 1=左手 */)
{
    SI2C_Dev_t dev = hand ? SI2C_MPU_LEFT : SI2C_MPU_RIGHT;
    uint8_t buf[14];
    if (SoftI2C_ReadBuf(dev, MPU6050_ADDR, MPU6050_ACCEL_XOUT_H, buf, 14) != SI2C_OK) {
        return 1;
    }
    MPU6050_t *m = hand ? &MPU6050_Left : &MPU6050_Right;
    m->Accel_X_RAW = i2c2s16(buf[0], buf[1]);
    m->Accel_Y_RAW = i2c2s16(buf[2], buf[3]);
    m->Accel_Z_RAW = i2c2s16(buf[4], buf[5]);
    m->Gyro_X_RAW  = i2c2s16(buf[8], buf[9]);
    m->Gyro_Y_RAW  = i2c2s16(buf[10], buf[11]);
    m->Gyro_Z_RAW  = i2c2s16(buf[12], buf[13]);

    /* 减去零漂 */
    m->Gyro_X_RAW -= m->Gyro_X_Offset;
    m->Gyro_Y_RAW -= m->Gyro_Y_Offset;
    m->Gyro_Z_RAW -= m->Gyro_Z_Offset;

    /* 转为物理量 */
    /* 加速度：±2g 量程 → 16384 LSB/g → ÷16384×9.8 m/s^2 */
    m->Ax = (float)m->Accel_X_RAW / 16384.0f * 9.8f;
    m->Ay = (float)m->Accel_Y_RAW / 16384.0f * 9.8f;
    m->Az = (float)m->Accel_Z_RAW / 16384.0f * 9.8f;

    /* 陀螺仪：±250°/s → 131 LSB/(°/s) → ÷131 → rad/s */
    m->Gx = (float)m->Gyro_X_RAW / 131.0f * 0.0174533f;
    m->Gy = (float)m->Gyro_Y_RAW / 131.0f * 0.0174533f;
    m->Gz = (float)m->Gyro_Z_RAW / 131.0f * 0.0174533f;

    return 0;
}

/* 内部：陀螺仪零漂校准 */
static void calibrate_gyro(MPU6050_t *m, SI2C_Dev_t dev)
{
    int32_t sum_x = 0, sum_y = 0, sum_z = 0;
    uint8_t buf[6];
    for (uint16_t i = 0; i < CALIB_SAMPLES; i++) {
        SoftI2C_ReadBuf(dev, MPU6050_ADDR, MPU6050_GYRO_XOUT_H, buf, 6);
        sum_x += i2c2s16(buf[0], buf[1]);
        sum_y += i2c2s16(buf[2], buf[3]);
        sum_z += i2c2s16(buf[4], buf[5]);
        HAL_Delay(1);
    }
    m->Gyro_X_Offset = (int16_t)(sum_x / CALIB_SAMPLES);
    m->Gyro_Y_Offset = (int16_t)(sum_y / CALIB_SAMPLES);
    m->Gyro_Z_Offset = (int16_t)(sum_z / CALIB_SAMPLES);
}

/* ── 公共 API ── */

uint8_t MPU6050_Init_Right(void)
{
    MPU6050_t *m = &MPU6050_Right;
    memset(m, 0, sizeof(MPU6050_t));

    /* 软复位 */
    SoftI2C_WriteByte(SI2C_MPU_RIGHT, MPU6050_ADDR, MPU6050_PWR_MGMT1, 0x80);
    HAL_Delay(100);

    /* 唤醒 */
    SoftI2C_WriteByte(SI2C_MPU_RIGHT, MPU6050_ADDR, MPU6050_PWR_MGMT1, 0x00);
    HAL_Delay(50);

    /* 验证 WHO_AM_I */
    uint8_t who = 0;
    if (SoftI2C_ReadByte(SI2C_MPU_RIGHT, MPU6050_ADDR, MPU6050_WHO_AM_I, &who) != SI2C_OK) {
        return 1;
    }
    if (who != 0x68) return 2;

    /* 陀螺仪零漂校准 */
    calibrate_gyro(m, SI2C_MPU_RIGHT);

    /* 记录开机零位（互补滤波从 0 开始）*/
    m->Pitch_Offset = 0.0f;
    m->Roll_Offset  = 0.0f;

    return 0;
}

uint8_t MPU6050_Init_Left(void)
{
    MPU6050_t *m = &MPU6050_Left;
    memset(m, 0, sizeof(MPU6050_t));

    SoftI2C_WriteByte(SI2C_MPU_LEFT, MPU6050_ADDR, MPU6050_PWR_MGMT1, 0x80);
    HAL_Delay(100);
    SoftI2C_WriteByte(SI2C_MPU_LEFT, MPU6050_ADDR, MPU6050_PWR_MGMT1, 0x00);
    HAL_Delay(50);

    uint8_t who = 0;
    if (SoftI2C_ReadByte(SI2C_MPU_LEFT, MPU6050_ADDR, MPU6050_WHO_AM_I, &who) != SI2C_OK) {
        return 1;
    }
    if (who != 0x68) return 2;

    calibrate_gyro(m, SI2C_MPU_LEFT);
    m->Pitch_Offset = 0.0f;
    m->Roll_Offset  = 0.0f;
    return 0;
}

uint8_t MPU6050_Read_All_Right(void)  { return read_all_raw(0); }
uint8_t MPU6050_Read_All_Left(void)   { return read_all_raw(1); }

/* ── 互补滤波姿态解算 ── */
void MPU6050_Compute_Attitude_Right(void)
{
    MPU6050_t *m = &MPU6050_Right;
    /* 加速度计估算角度 */
    float acc_pitch = atan2f(m->Ay, sqrtf(m->Ax * m->Ax + m->Az * m->Az)) * 57.29578f;
    float acc_roll  = atan2f(-m->Ax, m->Az) * 57.29578f;

    /* 互补滤波 */
    m->Pitch = ALPHA * (m->Pitch + m->Gx * 57.29578f * 0.01f) + (1.0f - ALPHA) * acc_pitch;
    m->Roll  = ALPHA * (m->Roll  + m->Gy * 57.29578f * 0.01f) + (1.0f - ALPHA) * acc_roll;
}

void MPU6050_Compute_Attitude_Left(void)
{
    MPU6050_t *m = &MPU6050_Left;
    float acc_pitch = atan2f(m->Ay, sqrtf(m->Ax * m->Ax + m->Az * m->Az)) * 57.29578f;
    float acc_roll  = atan2f(-m->Ax, m->Az) * 57.29578f;
    m->Pitch = ALPHA * (m->Pitch + m->Gx * 57.29578f * 0.01f) + (1.0f - ALPHA) * acc_pitch;
    m->Roll  = ALPHA * (m->Roll  + m->Gy * 57.29578f * 0.01f) + (1.0f - ALPHA) * acc_roll;
}

/* ── 三步跌倒检测 ── */
static uint8_t fall_detect(MPU6050_t *m)
{
    static uint8_t  state = 0;       /* 0=正常, 1=失重, 2=撞击 */
    static uint32_t t_freefall = 0;
    static uint32_t t_impact = 0;
    uint32_t now = HAL_GetTick();

    float a_mag = sqrtf(m->Ax * m->Ax + m->Ay * m->Ay + m->Az * m->Az);

    switch (state) {
    case 0: /* 正常 → 检测失重 */
        if (a_mag < 0.4f * 9.8f) {  /* < 0.4g */
            if (t_freefall == 0) t_freefall = now;
            if (now - t_freefall > 30) {  /* 失重持续 > 30ms */
                state = 1;
            }
        } else {
            t_freefall = 0;
        }
        break;

    case 1: /* 失重中 → 检测撞击 */
        if (a_mag > 3.0f * 9.8f) {  /* > 3g */
            t_impact = now;
            state = 2;
        }
        if (now - t_freefall > 2000) {  /* 超时回正常 */
            state = 0;
            t_freefall = 0;
        }
        break;

    case 2: /* 撞击后 → 检测静止 */
        if (now - t_impact > 10000) {  /* 10s 后检查 */
            if (a_mag < 1.2f * 9.8f && a_mag > 0.8f * 9.8f) {  /* 接近 1g = 静止躺地 */
                state = 0;
                return 1;  /* 跌倒确认 */
            }
            state = 0;  /* 恢复 */
        }
        break;
    }
    return 0;
}

uint8_t MPU6050_FallDetect_Right(void) { return fall_detect(&MPU6050_Right); }
uint8_t MPU6050_FallDetect_Left(void)  { return fall_detect(&MPU6050_Left); }
