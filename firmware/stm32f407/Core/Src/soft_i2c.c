#include "soft_i2c.h"
#include "gpio.h"

#define SI2C_DELAY_100K      50U
#define SI2C_DELAY_400K      12U
#define SI2C_TO_CYCLES       2000UL
#define SI2C_RECOVERY_CLKS   9U

#define SCL_H(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SCL_PORT->BSRR = SI2C1_SCL_PIN; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = SI2C2_SCL_PIN; \
    else                              SI2C3_SCL_PORT->BSRR = SI2C3_SCL_PIN; \
} while (0)

#define SCL_L(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SCL_PORT->BSRR = (uint32_t)SI2C1_SCL_PIN << 16U; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = (uint32_t)SI2C2_SCL_PIN << 16U; \
    else                              SI2C3_SCL_PORT->BSRR = (uint32_t)SI2C3_SCL_PIN << 16U; \
} while (0)

#define SDA_H(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SDA_PORT->BSRR = SI2C1_SDA_PIN; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = SI2C2_SDA_PIN; \
    else                              SI2C3_SDA_PORT->BSRR = SI2C3_SDA_PIN; \
} while (0)

#define SDA_L(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SDA_PORT->BSRR = (uint32_t)SI2C1_SDA_PIN << 16U; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = (uint32_t)SI2C2_SDA_PIN << 16U; \
    else                              SI2C3_SDA_PORT->BSRR = (uint32_t)SI2C3_SDA_PIN << 16U; \
} while (0)

#define SDA_READ(dev) ( \
    (dev) == SI2C_MPU_RIGHT ? ((SI2C1_SDA_PORT->IDR & SI2C1_SDA_PIN) ? 1U : 0U) : \
    (dev) == SI2C_MPU_LEFT  ? ((SI2C2_SDA_PORT->IDR & SI2C2_SDA_PIN) ? 1U : 0U) : \
                              ((SI2C3_SDA_PORT->IDR & SI2C3_SDA_PIN) ? 1U : 0U) )

#define SCL_READ(dev) ( \
    (dev) == SI2C_MPU_RIGHT ? ((SI2C1_SCL_PORT->IDR & SI2C1_SCL_PIN) ? 1U : 0U) : \
    (dev) == SI2C_MPU_LEFT  ? ((SI2C2_SCL_PORT->IDR & SI2C2_SCL_PIN) ? 1U : 0U) : \
                              ((SI2C3_SCL_PORT->IDR & SI2C3_SCL_PIN) ? 1U : 0U) )

static uint8_t pin_to_bit(uint16_t pin)
{
    if      (pin == GPIO_PIN_0)  return 0U;
    else if (pin == GPIO_PIN_1)  return 1U;
    else if (pin == GPIO_PIN_2)  return 2U;
    else if (pin == GPIO_PIN_3)  return 3U;
    else if (pin == GPIO_PIN_4)  return 4U;
    else if (pin == GPIO_PIN_5)  return 5U;
    else if (pin == GPIO_PIN_6)  return 6U;
    else if (pin == GPIO_PIN_7)  return 7U;
    else if (pin == GPIO_PIN_8)  return 8U;
    else if (pin == GPIO_PIN_9)  return 9U;
    else if (pin == GPIO_PIN_10) return 10U;
    else if (pin == GPIO_PIN_11) return 11U;
    else if (pin == GPIO_PIN_12) return 12U;
    else if (pin == GPIO_PIN_13) return 13U;
    else if (pin == GPIO_PIN_14) return 14U;
    else if (pin == GPIO_PIN_15) return 15U;
    return 0U;
}

static void SDA_SetOut(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t bit;

    if (dev == SI2C_MPU_RIGHT) {
        port = SI2C1_SDA_PORT;
        pin = SI2C1_SDA_PIN;
    } else if (dev == SI2C_MPU_LEFT) {
        port = SI2C2_SDA_PORT;
        pin = SI2C2_SDA_PIN;
    } else {
        port = SI2C3_SDA_PORT;
        pin = SI2C3_SDA_PIN;
    }

    bit = pin_to_bit(pin);
    port->MODER &= ~(3UL << (2UL * bit));
    port->MODER |=  (1UL << (2UL * bit));
}

static void SDA_SetIn(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t bit;

    if (dev == SI2C_MPU_RIGHT) {
        port = SI2C1_SDA_PORT;
        pin = SI2C1_SDA_PIN;
    } else if (dev == SI2C_MPU_LEFT) {
        port = SI2C2_SDA_PORT;
        pin = SI2C2_SDA_PIN;
    } else {
        port = SI2C3_SDA_PORT;
        pin = SI2C3_SDA_PIN;
    }

    bit = pin_to_bit(pin);
    port->MODER &= ~(3UL << (2UL * bit));
}

static uint32_t si2c_get_half_delay(SI2C_Dev_t dev)
{
    return (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;
}

static void si2c_delay(uint32_t count)
{
    __IO uint32_t delay = count;
    while (delay--) {
        __asm("nop");
    }
}

static uint8_t si2c_wait_ack(SI2C_Dev_t dev)
{
    uint32_t to = SI2C_TO_CYCLES;

    while (SDA_READ(dev) && (--to != 0U)) {
        __asm("nop");
    }

    return SDA_READ(dev) ? 1U : 0U;
}

static uint8_t si2c_wait_scl_high(SI2C_Dev_t dev)
{
    uint32_t to = SI2C_TO_CYCLES;

    while (!SCL_READ(dev) && (--to != 0U)) {
        __asm("nop");
    }

    return (to == 0U) ? 1U : 0U;
}

static void si2c_start(SI2C_Dev_t dev)
{
    uint32_t half = si2c_get_half_delay(dev);

    SDA_SetOut(dev);
    SDA_H(dev);
    SCL_H(dev);
    si2c_delay(half);
    SDA_L(dev);
    si2c_delay(half);
    SCL_L(dev);
    si2c_delay(half);
}

static void si2c_stop(SI2C_Dev_t dev)
{
    uint32_t half = si2c_get_half_delay(dev);

    SDA_SetOut(dev);
    SDA_L(dev);
    si2c_delay(half);
    SCL_H(dev);
    (void)si2c_wait_scl_high(dev);
    si2c_delay(half);
    SDA_H(dev);
    si2c_delay(half);
}

static void si2c_send_clock_pulse(SI2C_Dev_t dev)
{
    uint32_t half = si2c_get_half_delay(dev);

    SCL_L(dev);
    si2c_delay(half);
    SCL_H(dev);
    (void)si2c_wait_scl_high(dev);
    si2c_delay(half);
}

static uint8_t si2c_write_byte(SI2C_Dev_t dev, uint8_t byte)
{
    uint32_t half = si2c_get_half_delay(dev);
    uint8_t i;
    uint8_t nack;

    SDA_SetOut(dev);

    for (i = 0U; i < 8U; i++) {
        if ((byte & 0x80U) != 0U) {
            SDA_H(dev);
        } else {
            SDA_L(dev);
        }

        si2c_delay(half);
        SCL_H(dev);
        (void)si2c_wait_scl_high(dev);
        si2c_delay(half);
        SCL_L(dev);
        si2c_delay(half);
        byte <<= 1;
    }

    SDA_SetIn(dev);
    si2c_delay(half);
    SCL_H(dev);
    (void)si2c_wait_scl_high(dev);
    nack = si2c_wait_ack(dev);
    si2c_delay(half);
    SCL_L(dev);
    si2c_delay(half);
    SDA_SetOut(dev);

    return nack;
}

static uint8_t si2c_read_byte(SI2C_Dev_t dev, uint8_t send_ack)
{
    uint32_t half = si2c_get_half_delay(dev);
    uint8_t i;
    uint8_t byte = 0U;

    SDA_SetIn(dev);

    for (i = 0U; i < 8U; i++) {
        byte <<= 1;
        si2c_delay(half);
        SCL_H(dev);
        (void)si2c_wait_scl_high(dev);
        si2c_delay(half);
        if (SDA_READ(dev)) {
            byte |= 1U;
        }
        SCL_L(dev);
        si2c_delay(half);
    }

    SDA_SetOut(dev);
    if (send_ack != 0U) {
        SDA_L(dev);
    } else {
        SDA_H(dev);
    }

    si2c_delay(half);
    SCL_H(dev);
    (void)si2c_wait_scl_high(dev);
    si2c_delay(half);
    SCL_L(dev);
    si2c_delay(half);
    SDA_H(dev);

    return byte;
}

static SI2C_Status_t si2c_bus_recovery(SI2C_Dev_t dev)
{
    uint8_t i;
    uint32_t half = si2c_get_half_delay(dev);

    SCL_H(dev);
    si2c_delay(half);

    for (i = 0U; i < SI2C_RECOVERY_CLKS; i++) {
        if (SDA_READ(dev)) {
            break;
        }
        si2c_send_clock_pulse(dev);
    }

    if (!SDA_READ(dev)) {
        return SI2C_TIMEOUT;
    }

    si2c_stop(dev);
    return SI2C_OK;
}

void SoftI2C_Init(void)
{
    uint8_t d;

    for (d = 0U; d < 3U; d++) {
        SI2C_Dev_t dev = (SI2C_Dev_t)d;
        SDA_SetIn(dev);
        if (!SDA_READ(dev)) {
            (void)si2c_bus_recovery(dev);
        }
        si2c_stop(dev);
    }
}

SI2C_Status_t SoftI2C_ReadByte(SI2C_Dev_t dev, uint8_t dev_addr,
                               uint8_t reg_addr, uint8_t *data)
{
    uint32_t half = si2c_get_half_delay(dev);

    if (data == 0) {
        return SI2C_TIMEOUT;
    }

    si2c_start(dev);
    if (si2c_write_byte(dev, (uint8_t)(dev_addr & 0xFEU)) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    si2c_delay(half);
    if (si2c_write_byte(dev, reg_addr) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    si2c_start(dev);
    if (si2c_write_byte(dev, (uint8_t)(dev_addr | 0x01U)) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    *data = si2c_read_byte(dev, 0U);
    si2c_stop(dev);
    return SI2C_OK;
}

SI2C_Status_t SoftI2C_ReadBuf(SI2C_Dev_t dev, uint8_t dev_addr,
                              uint8_t reg_addr, uint8_t *buf, uint8_t len)
{
    uint8_t i;
    uint32_t half = si2c_get_half_delay(dev);

    if ((buf == 0) || (len == 0U)) {
        return SI2C_OK;
    }

    si2c_start(dev);
    if (si2c_write_byte(dev, (uint8_t)(dev_addr & 0xFEU)) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    si2c_delay(half);
    if (si2c_write_byte(dev, reg_addr) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    si2c_start(dev);
    if (si2c_write_byte(dev, (uint8_t)(dev_addr | 0x01U)) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    for (i = 0U; i < len; i++) {
        buf[i] = si2c_read_byte(dev, (i < (uint8_t)(len - 1U)) ? 1U : 0U);
    }

    si2c_stop(dev);
    return SI2C_OK;
}

SI2C_Status_t SoftI2C_WriteByte(SI2C_Dev_t dev, uint8_t dev_addr,
                                uint8_t reg_addr, uint8_t data)
{
    uint32_t half = si2c_get_half_delay(dev);

    si2c_start(dev);
    if (si2c_write_byte(dev, (uint8_t)(dev_addr & 0xFEU)) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    si2c_delay(half);
    if (si2c_write_byte(dev, reg_addr) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    if (si2c_write_byte(dev, data) != 0U) {
        si2c_stop(dev);
        return SI2C_NACK;
    }

    si2c_stop(dev);
    return SI2C_OK;
}

uint8_t SoftI2C_ProbeReg(SI2C_Dev_t dev, uint8_t dev_addr, uint8_t reg_addr)
{
    uint32_t half = si2c_get_half_delay(dev);

    si2c_start(dev);
    if (si2c_write_byte(dev, (uint8_t)(dev_addr & 0xFEU)) != 0U) {
        si2c_stop(dev);
        return 1U;
    }

    si2c_delay(half);
    if (si2c_write_byte(dev, reg_addr) != 0U) {
        si2c_stop(dev);
        return 2U;
    }

    si2c_stop(dev);
    return 0U;
}
