#include "soft_i2c.h"
#include "gpio.h"

#define SI2C_DELAY_100K  3   /* 100kHz 标准模式延时参数 */
#define SI2C_DELAY_400K  0   /* 400kHz 快速模式延时参数 */
#define SI2C_TIMEOUT_MAX 10000  /* 等待从设备释放 SDA 的超时上限 */

/* ── 内部辅助宏 ── */
#define SCL_H(dev)  do { \
    if((dev)==SI2C_MPU_RIGHT)      SI2C1_SCL_PORT->BSRR = SI2C1_SCL_PIN; \
    else if((dev)==SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = SI2C2_SCL_PIN; \
    else                            SI2C3_SCL_PORT->BSRR = SI2C3_SCL_PIN; \
} while(0)

#define SCL_L(dev)  do { \
    if((dev)==SI2C_MPU_RIGHT)      SI2C1_SCL_PORT->BSRR = (uint32_t)SI2C1_SCL_PIN << 16U; \
    else if((dev)==SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = (uint32_t)SI2C2_SCL_PIN << 16U; \
    else                            SI2C3_SCL_PORT->BSRR = (uint32_t)SI2C3_SCL_PIN << 16U; \
} while(0)

#define SDA_H(dev)  do { \
    if((dev)==SI2C_MPU_RIGHT)      SI2C1_SDA_PORT->BSRR = SI2C1_SDA_PIN; \
    else if((dev)==SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = SI2C2_SDA_PIN; \
    else                            SI2C3_SDA_PORT->BSRR = SI2C3_SDA_PIN; \
} while(0)

#define SDA_L(dev)  do { \
    if((dev)==SI2C_MPU_RIGHT)      SI2C1_SDA_PORT->BSRR = (uint32_t)SI2C1_SDA_PIN << 16U; \
    else if((dev)==SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = (uint32_t)SI2C2_SDA_PIN << 16U; \
    else                            SI2C3_SDA_PORT->BSRR = (uint32_t)SI2C3_SDA_PIN << 16U; \
} while(0)

#define SDA_READ(dev) ( \
    (dev)==SI2C_MPU_RIGHT ? ((SI2C1_SDA_PORT->IDR & SI2C1_SDA_PIN) ? 1 : 0) : \
    (dev)==SI2C_MPU_LEFT  ? ((SI2C2_SDA_PORT->IDR & SI2C2_SDA_PIN) ? 1 : 0) : \
                             ((SI2C3_SDA_PORT->IDR & SI2C3_SDA_PIN) ? 1 : 0) )

/* ── GPIO 位号查找表（替代 GCC 内建 __builtin_ctz） ── */
static uint8_t pin_to_bit(uint16_t pin) {
    if      (pin == GPIO_PIN_0)  return 0;
    else if (pin == GPIO_PIN_1)  return 1;
    else if (pin == GPIO_PIN_2)  return 2;
    else if (pin == GPIO_PIN_3)  return 3;
    else if (pin == GPIO_PIN_4)  return 4;
    else if (pin == GPIO_PIN_5)  return 5;
    else if (pin == GPIO_PIN_6)  return 6;
    else if (pin == GPIO_PIN_7)  return 7;
    else if (pin == GPIO_PIN_8)  return 8;
    else if (pin == GPIO_PIN_9)  return 9;
    else if (pin == GPIO_PIN_10) return 10;
    else if (pin == GPIO_PIN_11) return 11;
    else if (pin == GPIO_PIN_12) return 12;
    else if (pin == GPIO_PIN_13) return 13;
    else if (pin == GPIO_PIN_14) return 14;
    else if (pin == GPIO_PIN_15) return 15;
    return 0;
}

/* ── GPIO 方向切换 ── */
static void SDA_SetOut(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    if (dev == SI2C_MPU_RIGHT)      { port = SI2C1_SDA_PORT; pin = SI2C1_SDA_PIN; }
    else if (dev == SI2C_MPU_LEFT)  { port = SI2C2_SDA_PORT; pin = SI2C2_SDA_PIN; }
    else                             { port = SI2C3_SDA_PORT; pin = SI2C3_SDA_PIN; }
    uint8_t bit = pin_to_bit(pin);
    port->MODER &= ~(3U << (2U * bit));
    port->MODER |=  (1U << (2U * bit));
}

static void SDA_SetIn(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    if (dev == SI2C_MPU_RIGHT)      { port = SI2C1_SDA_PORT; pin = SI2C1_SDA_PIN; }
    else if (dev == SI2C_MPU_LEFT)  { port = SI2C2_SDA_PORT; pin = SI2C2_SDA_PIN; }
    else                             { port = SI2C3_SDA_PORT; pin = SI2C3_SDA_PIN; }
    uint8_t bit = pin_to_bit(pin);
    port->MODER &= ~(3U << (2U * bit));
}

/* ── 延时 ── */
static void si2c_delay(uint32_t n)
{
    /* 约 (n+1)*5 个 NOP，168MHz 下每次循环约 30ns */
    for (volatile uint32_t i = 0; i < n; i++) {
        __NOP();
    }
}

/* ── 启动条件 ── */
static void si2c_start(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    SDA_H(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SDA_L(dev);
    si2c_delay(speed_delay);
    SCL_L(dev);
    si2c_delay(speed_delay);
}

/* ── 停止条件 ── */
static void si2c_stop(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    SDA_L(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SDA_H(dev);
    si2c_delay(speed_delay);
}

/* ── 写一个字节，返回 ACK (0=ACK, 1=NACK) ── */
static uint8_t si2c_write_byte(SI2C_Dev_t dev, uint8_t byte, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    for (uint8_t i = 0; i < 8; i++) {
        if (byte & 0x80) SDA_H(dev);
        else             SDA_L(dev);
        si2c_delay(speed_delay);
        SCL_H(dev);
        si2c_delay(speed_delay);
        SCL_L(dev);
        byte <<= 1;
    }
    /* 第九个时钟：读 ACK */
    SDA_SetIn(dev);
    SDA_H(dev); /* 释放 SDA，让从设备拉低 */
    si2c_delay(speed_delay);
    SCL_H(dev);
    uint8_t ack = SDA_READ(dev);
    si2c_delay(speed_delay);
    SCL_L(dev);
    SDA_SetOut(dev);
    return ack;
}

/* ── 读一个字节，send_ack=0 发 ACK，=1 发 NACK ── */
static uint8_t si2c_read_byte(SI2C_Dev_t dev, uint8_t send_ack, uint8_t speed_delay)
{
    uint8_t byte = 0;
    SDA_SetIn(dev);
    for (uint8_t i = 0; i < 8; i++) {
        SDA_H(dev);
        si2c_delay(speed_delay);
        SCL_H(dev);
        byte <<= 1;
        if (SDA_READ(dev)) byte |= 1;
        si2c_delay(speed_delay);
        SCL_L(dev);
    }
    /* 第九个时钟：发送 ACK/NACK */
    SDA_SetOut(dev);
    if (send_ack) SDA_L(dev);  /* ACK  = 拉低 */
    else          SDA_H(dev);  /* NACK = 拉高 */
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SCL_L(dev);
    SDA_H(dev);
    return byte;
}

/* ── 公共 API ── */
void SoftI2C_Init(void)
{
    /* 引脚已在 MX_GPIO_Init() 中配置为推挽输出高电平 */
    /* 此处可添加一次初始 STOP 确保总线空闲 */
    for (uint8_t d = 0; d < 3; d++) {
        si2c_stop((SI2C_Dev_t)d, SI2C_DELAY_100K);
    }
}

SI2C_Status_t SoftI2C_ReadByte(SI2C_Dev_t dev, uint8_t dev_addr,
                               uint8_t reg_addr, uint8_t *data)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* ── 写阶段：器件地址 + 寄存器地址 ── */
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFE, speed)) {  /* 写：bit0=0 */
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }

    /* ── 读阶段：器件地址 + 读 1 字节 ── */
    si2c_start(dev, speed);  /* Repeated Start */
    if (si2c_write_byte(dev, dev_addr | 0x01, speed)) {  /* 读：bit0=1 */
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    *data = si2c_read_byte(dev, 0, speed);  /* 0 = NACK（读完最后一个字节） */
    si2c_stop(dev, speed);
    return SI2C_OK;
}

SI2C_Status_t SoftI2C_ReadBuf(SI2C_Dev_t dev, uint8_t dev_addr,
                              uint8_t reg_addr, uint8_t *buf, uint8_t len)
{
    if (len == 0) return SI2C_OK;
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* 写阶段 */
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFE, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }

    /* Repeated Start → 读阶段 */
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr | 0x01, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }

    for (uint8_t i = 0; i < len; i++) {
        /* 最后一个字节发 NACK (0)，前面的发 ACK (1) */
        buf[i] = si2c_read_byte(dev, (i < len - 1) ? 1 : 0, speed);
    }
    si2c_stop(dev, speed);
    return SI2C_OK;
}

SI2C_Status_t SoftI2C_WriteByte(SI2C_Dev_t dev, uint8_t dev_addr,
                                uint8_t reg_addr, uint8_t data)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFE, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, data, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    si2c_stop(dev, speed);
    return SI2C_OK;
}
