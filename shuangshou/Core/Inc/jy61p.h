/**
  ******************************************************************************
  * @file           : jy61p.h
  * @brief          : 维特智能 JY61P 高精度 IMU 驱动 — 硬件 I2C 模式
  ******************************************************************************
  */

#ifndef __JY61P_H
#define __JY61P_H

#include "stdint.h"

/* ── I2C 地址 ── */
#define JY61P_I2C_ADDR    0xA0U   /* 8-bit 写地址 (7-bit: 0x50 << 1) */

/* ── I2C 通道编号 ── */
#define JY61P_CH_RIGHT    1U      /* I2C1: PB6/PB7 */
#define JY61P_CH_LEFT     2U      /* I2C2: PB10/PB11 */

/* ── 寄存器映射 ── */
#define JY61P_REG_AX_L      0x34U
#define JY61P_REG_GX_L      0x37U
#define JY61P_REG_ROLL_L    0x3DU

/* ── 连读字节数 ── */
#define JY61P_BURST_ACC_GYRO_LEN  12U
#define JY61P_BURST_ANGLE_LEN      6U

/* ── 转换常数 ── */
#define JY61P_ACC_SCALE      0.00478515625f
#define JY61P_GYRO_SCALE     0.06103515625f
#define JY61P_ANGLE_SCALE    0.0054931640625f

/* ── 数据结构 ── */
typedef struct {
    float    acc[3];
    float    gyro[3];
    float    angle[3];
    int16_t  acc_raw[3];
    int16_t  gyro_raw[3];
    int16_t  angle_raw[3];
    uint8_t  online;
} JY61P_Data_t;

extern JY61P_Data_t JY61P_Right;
extern JY61P_Data_t JY61P_Left;

/* ── API ── */
uint8_t JY61P_Init(uint8_t channel);
uint8_t JY61P_Read_Data(uint8_t channel, float *acc, float *gyro, float *angle);
uint8_t JY61P_Read_Angle(uint8_t channel, float *roll, float *pitch, float *yaw);
uint8_t JY61P_IsOnline(uint8_t channel);
void    JY61P_GetLastAngle(uint8_t channel, float *roll, float *pitch, float *yaw);

#endif /* __JY61P_H */
