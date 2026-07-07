/**
  ******************************************************************************
  * @file           : i2c.h
  * @brief          : Hardware I2C handles and init (I2C1/I2C2/I2C3)
  ******************************************************************************
  */
#ifndef __I2C_H
#define __I2C_H

#include "stm32f4xx_hal.h"

/* I2C1: PB6=SCL, PB7=SDA  -> 右手 JY61P */
/* I2C2: PB10=SCL, PB11=SDA -> 左手 JY61P */
/* I2C3: PA8=SCL, PC9=SDA   -> MAX30102   */
extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c2;
extern I2C_HandleTypeDef hi2c3;

void MX_I2C1_Init(void);
void MX_I2C2_Init(void);
void MX_I2C3_Init(void);

#endif /* __I2C_H */
