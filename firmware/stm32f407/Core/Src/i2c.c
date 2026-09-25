/**
  ******************************************************************************
  * @file           : i2c.c
  * @brief          : Hardware I2C initialization — I2C1/2/3 with bus recovery
  ******************************************************************************
  */
#include "i2c.h"
#include "gpio.h"

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;
I2C_HandleTypeDef hi2c3;

/* Bit-bang bus recovery: 9 SCL pulses + STOP to unstuck hung slaves */
static void i2c_bus_recover(GPIO_TypeDef *scl_port, uint16_t scl_pin,
                            GPIO_TypeDef *sda_port, uint16_t sda_pin)
{
    GPIO_InitTypeDef gpio = {0};
    int i;

    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    gpio.Pin = scl_pin;
    HAL_GPIO_Init(scl_port, &gpio);
    gpio.Pin = sda_pin;
    HAL_GPIO_Init(sda_port, &gpio);

    /* Idle: SCL=HIGH, SDA=HIGH */
    HAL_GPIO_WritePin(scl_port, scl_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(sda_port, sda_pin, GPIO_PIN_SET);
    for (i = 0; i < 50; i++) __asm("nop");

    /* 9 clock pulses */
    for (i = 0; i < 9; i++) {
        HAL_GPIO_WritePin(scl_port, scl_pin, GPIO_PIN_RESET);
        for (volatile int d = 0; d < 200; d++) __asm("nop");
        HAL_GPIO_WritePin(scl_port, scl_pin, GPIO_PIN_SET);
        for (volatile int d = 0; d < 200; d++) __asm("nop");
    }

    /* STOP condition: SDA LOW -> SCL HIGH -> SDA HIGH */
    HAL_GPIO_WritePin(sda_port, sda_pin, GPIO_PIN_RESET);
    for (volatile int d = 0; d < 200; d++) __asm("nop");
    HAL_GPIO_WritePin(scl_port, scl_pin, GPIO_PIN_SET);
    for (volatile int d = 0; d < 200; d++) __asm("nop");
    HAL_GPIO_WritePin(sda_port, sda_pin, GPIO_PIN_SET);
    for (volatile int d = 0; d < 200; d++) __asm("nop");
}

/* Peripheral reset: enable clock first, then force/release */
static void i2c_periph_reset(I2C_TypeDef *instance)
{
    if (instance == I2C1) {
        __HAL_RCC_I2C1_CLK_ENABLE();
        __HAL_RCC_I2C1_FORCE_RESET();
        for (volatile int d = 0; d < 200; d++) __asm("nop");
        __HAL_RCC_I2C1_RELEASE_RESET();
    } else if (instance == I2C2) {
        __HAL_RCC_I2C2_CLK_ENABLE();
        __HAL_RCC_I2C2_FORCE_RESET();
        for (volatile int d = 0; d < 200; d++) __asm("nop");
        __HAL_RCC_I2C2_RELEASE_RESET();
    } else if (instance == I2C3) {
        __HAL_RCC_I2C3_CLK_ENABLE();
        __HAL_RCC_I2C3_FORCE_RESET();
        for (volatile int d = 0; d < 200; d++) __asm("nop");
        __HAL_RCC_I2C3_RELEASE_RESET();
    }
}

static void MX_I2C_Init(I2C_HandleTypeDef *hi2c, I2C_TypeDef *instance,
                        uint32_t clock_speed_hz)
{
    hi2c->Instance = instance;
    hi2c->Init.ClockSpeed = clock_speed_hz;
    hi2c->Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c->Init.OwnAddress1 = 0;
    hi2c->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c->Init.OwnAddress2 = 0;
    hi2c->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(hi2c) != HAL_OK) {
        Error_Handler();
    }
}

void MX_I2C1_Init(void)
{
    i2c_periph_reset(I2C1);
    i2c_bus_recover(GPIOB, GPIO_PIN_6, GPIOB, GPIO_PIN_7);
    MX_I2C_Init(&hi2c1, I2C1, 50000U);
}

void MX_I2C2_Init(void)
{
    i2c_periph_reset(I2C2);
    i2c_bus_recover(GPIOB, GPIO_PIN_10, GPIOB, GPIO_PIN_11);
    MX_I2C_Init(&hi2c2, I2C2, 100000U);
}

void MX_I2C3_Init(void)
{
    /* MAX30102: clean init — no recovery needed on fresh pins */
    __HAL_RCC_I2C3_CLK_ENABLE();
    MX_I2C_Init(&hi2c3, I2C3, 100000U);
}
