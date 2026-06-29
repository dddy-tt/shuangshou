#ifndef __SOFT_I2C_H
#define __SOFT_I2C_H

#include "stdint.h"
#include "main.h"

/*
 * v3.1 active mapping:
 *   SoftI2C_1: PB6=SCL, PB7=SDA  -> 右手 JY61P
 *   SoftI2C_2: PB8=SCL, PB9=SDA  -> 左手 JY61P
 *   SoftI2C_3: PC6=SCL, PC7=SDA  -> MAX30102
 *
 * 注意：枚举名中的 MPU 仅为历史命名保留，当前项目不再使用 MPU6050。
 */

/* 右手 JY61P: PB6=SCL, PB7=SDA */
#define SI2C1_SCL_PORT    GPIOB
#define SI2C1_SCL_PIN     GPIO_PIN_6
#define SI2C1_SDA_PORT    GPIOB
#define SI2C1_SDA_PIN     GPIO_PIN_7

/* 左手 JY61P: PB8=SCL, PB9=SDA */
#define SI2C2_SCL_PORT    GPIOB
#define SI2C2_SCL_PIN     GPIO_PIN_8
#define SI2C2_SDA_PORT    GPIOB
#define SI2C2_SDA_PIN     GPIO_PIN_9

/* MAX30102: PC6=SCL, PC7=SDA */
#define SI2C3_SCL_PORT    GPIOC
#define SI2C3_SCL_PIN     GPIO_PIN_6
#define SI2C3_SDA_PORT    GPIOC
#define SI2C3_SDA_PIN     GPIO_PIN_7

typedef enum {
    SI2C_OK = 0,
    SI2C_TIMEOUT = 1,
    SI2C_NACK = 2,
} SI2C_Status_t;

typedef enum {
    SI2C_MPU_RIGHT = 0,  /* 历史命名保留: 当前对应右手 JY61P 总线 */
    SI2C_MPU_LEFT  = 1,  /* 历史命名保留: 当前对应左手 JY61P 总线 */
    SI2C_MAX30102  = 2,
} SI2C_Dev_t;

/*
 * 软件 I2C 初始化：在 MX_GPIO_Init() 之后调用。
 * 会尝试恢复总线空闲状态，并补发 STOP，避免上电后从设备卡死。
 */
void SoftI2C_Init(void);

/* 读取单个寄存器字节。返回 SI2C_OK / SI2C_TIMEOUT / SI2C_NACK。 */
SI2C_Status_t SoftI2C_ReadByte(SI2C_Dev_t dev, uint8_t dev_addr,
                               uint8_t reg_addr, uint8_t *data);

/* 从连续寄存器批量读取 len 个字节到 buf。 */
SI2C_Status_t SoftI2C_ReadBuf(SI2C_Dev_t dev, uint8_t dev_addr,
                              uint8_t reg_addr, uint8_t *buf, uint8_t len);

/* 向单个寄存器写入一个字节。 */
SI2C_Status_t SoftI2C_WriteByte(SI2C_Dev_t dev, uint8_t dev_addr,
                                uint8_t reg_addr, uint8_t data);

#endif /* __SOFT_I2C_H */
