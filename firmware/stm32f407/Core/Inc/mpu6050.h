/*
 * Legacy notice:
 * This header is kept only for compatibility/reference.
 * The active v3.1 IMU path is JY61P, not MPU6050.
 * New code should include jy61p.h instead of this file.
 */
#ifndef __MPU6050_H
#define __MPU6050_H

#include "stdint.h"

/* ── MPU6050 寄存器地址 ── */
#define MPU6050_ADDR      0xD0  /* 7-bit: 0x68, 8-bit: 0xD0 */
#define MPU6050_WHO_AM_I  0x75
#define MPU6050_PWR_MGMT1 0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B
#define MPU6050_GYRO_XOUT_H  0x43

/* ── 传感器数据结构 ── */
typedef struct {
    /* 原始 ADC 值 */
    int16_t Accel_X_RAW, Accel_Y_RAW, Accel_Z_RAW;
    int16_t Gyro_X_RAW,  Gyro_Y_RAW,  Gyro_Z_RAW;

    /* 物理量：加速度 m/s^2, 角速度 rad/s */
    float Ax, Ay, Az;
    float Gx, Gy, Gz;

    /* 姿态角（互补滤波输出）*/
    float Pitch;  /* 俯仰角，° */
    float Roll;   /* 横滚角，° */

    /* 开机零位（用于相对姿态）*/
    float Pitch_Offset;
    float Roll_Offset;

    /* 陀螺仪零漂 */
    int16_t Gyro_X_Offset, Gyro_Y_Offset, Gyro_Z_Offset;
} MPU6050_t;

extern MPU6050_t MPU6050_Right;
extern MPU6050_t MPU6050_Left;

/* ── API ── */
uint8_t MPU6050_Init_Right(void);
uint8_t MPU6050_Init_Left(void);

/*
 * 读取全部原始数据（加速度 + 陀螺仪，共 14 字节）
 * 返回 0=成功, 非0=通信失败
 */
uint8_t MPU6050_Read_All_Right(void);
uint8_t MPU6050_Read_All_Left(void);

/*
 * 姿态解算（互补滤波，Pitch/Roll）
 * 调用前提：已执行 Read_All
 */
void MPU6050_Compute_Attitude_Right(void);
void MPU6050_Compute_Attitude_Left(void);

/*
 * 三步跌倒检测
 * 返回 0=正常, 1=检测到跌倒
 */
uint8_t MPU6050_FallDetect_Right(void);
uint8_t MPU6050_FallDetect_Left(void);

#endif /* __MPU6050_H */
