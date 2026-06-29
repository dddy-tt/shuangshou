/**
  ******************************************************************************
  * @file           : soft_uart.c
  * @brief          : 软件 UART TX-only — 115200bps, AT+MQTTPUB 协议
  * @author         : v3.0 → v3.1 Gemini 评审修正
  * @date           : 2026-06-29
  *
  * @修复记录:
  *   盲点 2: 二进制 MQTT 裸帧 ESP-01S AT 固件无法解析
  *           → 改用 ASCII AT+MQTTPUB=0,"topic","payload",0,0
  *   盲点 3: 9600bps → 115200bps
  *           DWT: 168e6/115200 = 1458.33 → 1458 cycles/bit
  *           全字节发送关中断 (__disable_irq) 保护波形
  *           45 字节命令 ≈ 3.9ms 全盲期, 不压穿 5ms 槽
  *
  * @ESP-01S 前置 AT 配置 (一次性):
  *   AT+CWMODE=1
  *   AT+CWJAP="SSID","PASSWORD"
  *   AT+MQTTUSERCFG=0,1,"","","",0,0,""
  *   AT+MQTTCONN=0,"broker.emqx.io",1883,0
  *
  *   之后 F407 发来的 AT+MQTTPUB 命令会被 ESP-01S 直接执行
  ******************************************************************************
  */

#include "soft_uart.h"
#include "stm32f4xx_hal.h"
#include "string.h"
#include "stdio.h"

/* ── 位延时: 168MHz / 115200bps = 1458.33 → 1458 cycles ── */
#define BIT_CYCLES  1458U

/* ── MQTT 命令缓冲 ── */
#define AT_BUF_SIZE  96U

/* ── 位延时 ── */
static void delay_bit(void)
{
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < BIT_CYCLES) {
        __NOP();
    }
}

/* ── 发送 1 字节 (关中断, 波形不被打断) ── */
static void send_byte(uint8_t byte)
{
    /* 起始位: 拉低 */
    SOFTUART_TX_PORT->BSRR = (uint32_t)SOFTUART_TX_PIN << 16U;
    delay_bit();
    /* 8 数据位, LSB first */
    for (uint8_t i = 0; i < 8U; i++) {
        if (byte & 0x01U) SOFTUART_TX_PORT->BSRR = SOFTUART_TX_PIN;
        else              SOFTUART_TX_PORT->BSRR = (uint32_t)SOFTUART_TX_PIN << 16U;
        delay_bit();
        byte >>= 1U;
    }
    /* 停止位: 拉高 */
    SOFTUART_TX_PORT->BSRR = SOFTUART_TX_PIN;
    delay_bit();
}

/* ── 发送缓冲 (关总中断包裹, 防硬件打断波形) ── */
static void send_buf(const uint8_t *buf, uint16_t len)
{
    __disable_irq();  /* ★ 软串口关键段: 禁止任何中断打断 GPIO 翻转时序 */
    for (uint16_t i = 0; i < len; i++) {
        send_byte(buf[i]);
    }
    __enable_irq();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  初始化
 * ═══════════════════════════════════════════════════════════════════════════ */

void SoftUART_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOE_CLK_ENABLE();
    g.Pin   = SOFTUART_TX_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;  /* 115200bps 需要高速翻转 */
    HAL_GPIO_Init(SOFTUART_TX_PORT, &g);
    SOFTUART_TX_PORT->BSRR = SOFTUART_TX_PIN;

    /* 使能 DWT 周期计数器 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ★ AT+MQTTPUB 指令发送
 *
 *  格式: AT+MQTTPUB=0,"topic","payload",0,0\r\n
 *  示例: AT+MQTTPUB=0,"glove/light","ON",0,0\r\n
 *
 *  字节数: 前缀 17B + topic 长度 + 4B 引号逗号 + payload + 后缀 6B
 *         典型: 17+11+4+2+6 = 40B → @115200 ≈ 3.47ms
 * ═══════════════════════════════════════════════════════════════════════════ */

void SoftUART_SendMQTT(const char *topic, const char *payload)
{
    char at[AT_BUF_SIZE];
    int len = snprintf(at, sizeof(at),
                       "AT+MQTTPUB=0,\"%s\",\"%s\",0,0\r\n",
                       topic, payload);
    if (len > 0 && len < (int)sizeof(at)) {
        send_buf((const uint8_t *)at, (uint16_t)len);
    }
}
