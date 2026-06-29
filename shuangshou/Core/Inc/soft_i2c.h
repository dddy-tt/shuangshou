#ifndef __SOFT_I2C_H
#define __SOFT_I2C_H

#include "stdint.h"

/* ── 引脚宏定义 ── */
/* 右手 MPU6050: PB6=SCL, PB7=SDA */
#define SI2C1_SCL_PORT    GPIOB
#define SI2C1_SCL_PIN     GPIO_PIN_6
#define SI2C1_SDA_PORT    GPIOB
#define SI2C1_SDA_PIN     GPIO_PIN_7

/* 左手 MPU6050: PB8=SCL, PB9=SDA */
#define SI2C2_SCL_PORT    GPIOB
#define SI2C2_SCL_PIN     GPIO_PIN_8
#define SI2C2_SDA_PORT    GPIOB
#define SI2C2_SDA_PIN     GPIO_PIN_9

/* MAX30102: PC6=SCL, PC7=SDA */
#define SI2C3_SCL_PORT    GPIOC
#define SI2C3_SCL_PIN     GPIO_PIN_6
#define SI2C3_SDA_PORT    GPIOC
#define SI2C3_SDA_PIN     GPIO_PIN_7

/* ── 时序参数（168MHz 校准值） ── */
/* 100kHz 标准模式：半周期 5μs ≈ 840 CPU cycles */
/* 400kHz 快速模式：半周期 1.25μs ≈ 210 CPU cycles */

/* 100kHz 用 delay(3) ≈ 有效半周期 ~3.6μs（接近 100kHz） */
/* 400kHz 用 delay(0) = 空函数  ≈ 有效半周期 ~0.9μs */
/* 经过阈值测试后确定以上数值 */

/* ── API ── */
typedef enum {
    SI2C_OK = 0,
    SI2C_TIMEOUT = 1,
    SI2C_NACK = 2,
} SI2C_Status_t;

/* 三路软 I2C 设备编号，用于统一接口 */
typedef enum {
    SI2C_MPU_RIGHT = 0,  /* PB6/PB7 */
    SI2C_MPU_LEFT  = 1,  /* PB8/PB9 */
    SI2C_MAX30102  = 2,  /* PC6/PC7 */
} SI2C_Dev_t;

/*
 * 初始化：CubeMX 已配 GPIO 为推挽输出高电平，此函数仅做确认
 * 调用时机：main() 中 MX_GPIO_Init() 之后
 */
void SoftI2C_Init(void);

/*
 * 读取单字节（器件地址 + 寄存器地址 → 1 字节数据）
 * 返回 SI2C_OK / SI2C_TIMEOUT / SI2C_NACK
 */
SI2C_Status_t SoftI2C_ReadByte(SI2C_Dev_t dev, uint8_t dev_addr,
                               uint8_t reg_addr, uint8_t *data);

/*
 * 批量读取（器件地址 + 寄存器地址 → N 字节存入 buf）
 */
SI2C_Status_t SoftI2C_ReadBuf(SI2C_Dev_t dev, uint8_t dev_addr,
                              uint8_t reg_addr, uint8_t *buf, uint8_t len);

/*
 * 写单字节（器件地址 + 寄存器地址 + 1 字节数据）
 */
SI2C_Status_t SoftI2C_WriteByte(SI2C_Dev_t dev, uint8_t dev_addr,
                                uint8_t reg_addr, uint8_t data);

#endif /* __SOFT_I2C_H */
