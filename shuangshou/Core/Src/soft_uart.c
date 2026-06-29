/**
  ******************************************************************************
  * @file           : soft_uart.c
  * @brief          : 软件模拟 UART TX-only — DWT 精确定时
  * @author         : v3.0 IoT 控制子系统
  * @date           : 2026-06-29
  *
  * @原理:
  *   用 STM32 的 DWT (Data Watchpoint and Trace) 周期计数器做位延时。
  *   HAL_GetTick 精度仅 1ms, 无法满足 9600bps 的 104μs/bit。
  *   DWT->CYCCNT 每 1 个 CPU 时钟周期递增 1 次 (168MHz → 5.95ns 分辨率),
  *   用于 104μs 级延时绰绰有余。
  *
  * @为什么 TX-only:
  *   ESP-01S 只需要接收 F407 发来的 MQTT AT 指令, 不需要向 F407 发数据。
  *   TX-only 省去 EXTI 中断 + 定时器捕获 + 环形缓冲 ~200 行代码。
  *
  * @MQTT 帧格式 (只支持 QoS 0, 最小开销):
  *   Byte 0:     0x30 (PUBLISH, DUP=0, QoS=0, Retain=0)
  *   Byte 1:     Remaining Length = 2 + strlen(topic) + strlen(payload)
  *   Byte 2-3:   Topic Length (MSB first)
  *   Byte 4...:  Topic (UTF-8)
  *   Byte ...:   Payload (UTF-8)
  *
  * @ESP-01S 前置配置 (一次性, 用电脑 USB-TTL + AT 指令):
  *   AT+CWMODE=1           // Station 模式
  *   AT+CWJAP="SSID","PWD" // 连 WiFi
  *   AT+MQTTUSERCFG=0,1,"","","",0,0,""  // MQTT 用户配置
  *   AT+MQTTCONN=0,"broker.emqx.io",1883,0  // 连 MQTT Broker
  *   之后 F407 发来的裸 MQTT PUBLISH 帧会被 ESP-01S 直接转发到 Broker
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "soft_uart.h"
#include "stm32f4xx_hal.h"
#include "string.h"
#include "stdio.h"

/* ── 位延时参数 ──────────────────────────────────────────────────────────── */

/**
 * @brief 9600bps 下每位持续 104.17μs
 * @note  168MHz (每秒 1.68 亿周期) × 104.17μs = 17500 周期
 *       加上函数调用开销 ~50 cycles → 实际取 17400 (略微偏快)
 */
#define BIT_CYCLES  17400U

/**
 * @brief 最大 MQTT topic + payload 长度
 */
#define MQTT_TOPIC_MAX  32U
#define MQTT_PAYLOAD_MAX 16U

/* ═══════════════════════════════════════════════════════════════════════════
 *  位延时 — DWT 周期计数器
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  阻塞等待 BIT_CYCLES 个 CPU 周期
 * @note   DWT->CYCCNT 是 32-bit 自由运行计数器, 溢出后回绕。
 *         本函数的减法 (now - start) 对溢出免疫 (无符号整数回绕)
 */
static void delay_bit(void)
{
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < BIT_CYCLES) {
        __NOP();  /* 等, 不给编译器优化掉 */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  初始化
 * ═══════════════════════════════════════════════════════════════════════════ */

void SoftUART_Init(void)
{
    /* ── PE0 推挽输出, 初始高 (空闲位) ── */
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOE_CLK_ENABLE();
    g.Pin   = SOFTUART_TX_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_MEDIUM;  /* 中速够用 (9600bps), 省 EMI */
    HAL_GPIO_Init(SOFTUART_TX_PORT, &g);
    /* 确保初始高电平 */
    SOFTUART_TX_PORT->BSRR = SOFTUART_TX_PIN;

    /* ── 使能 DWT 周期计数器 ── */
    /* TRCENA (bit 24) = 使能 DWT/ITM/ETM/TPIU 调试组件 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    /* CYCCNTENA (bit 0) = 启动周期计数器 */
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  发送原语
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  发送 1 字节 (8N1 — 1 起始位 + 8 数据位 + 1 停止位)
 * @note   总耗时: 10 bits × 104μs ≈ 1.04ms
 *         仅在 50ms 任务中调用, 不阻塞实时 5ms/10ms 槽
 */
void SoftUART_SendByte(uint8_t byte)
{
    /* ── 起始位 (拉低) ── */
    SOFTUART_TX_PORT->BSRR = (uint32_t)SOFTUART_TX_PIN << 16U;
    delay_bit();

    /* ── 8 数据位, LSB first ── */
    for (uint8_t i = 0; i < 8U; i++) {
        if (byte & 0x01U) {
            SOFTUART_TX_PORT->BSRR = SOFTUART_TX_PIN;
        } else {
            SOFTUART_TX_PORT->BSRR = (uint32_t)SOFTUART_TX_PIN << 16U;
        }
        delay_bit();
        byte >>= 1U;
    }

    /* ── 停止位 (拉高) ── */
    SOFTUART_TX_PORT->BSRR = SOFTUART_TX_PIN;
    delay_bit();
}

void SoftUART_SendBuf(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        SoftUART_SendByte(buf[i]);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ★ MQTT PUBLISH 帧构造 + 发送
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  构造 MQTT PUBLISH 帧 (QoS 0, 无 Retain) 并发送
 *
 * @帧格式 (MQTT v3.1.1):
 *   Byte 0:      Fixed Header (0x30 = PUBLISH, DUP=0, QoS=0, RETAIN=0)
 *   Byte 1:      Remaining Length (编码为 1~4 字节; 本项目 ≤127 → 单字节)
 *   Byte 2..3:   Topic Length (uint16, MSB first)
 *   Byte 4..N:   Topic (UTF-8 字符串)
 *   Byte N+1..M: Payload (任意字节序列)
 *
 *   无 Packet Identifier (QoS 0 不需要)
 *
 * @note  ESP-01S 需预配为透传模式或 MQTT AT 固件,
 *        确保它把收到的 UART 数据直接当作 MQTT 帧发出。
 */
void SoftUART_SendMQTT(const char *topic, const char *payload)
{
    uint16_t tlen = (uint16_t)strlen(topic);
    uint16_t plen = (uint16_t)strlen(payload);
    uint16_t rlen = 2U + tlen + plen;  /* Remaining Length = TopicLen(2) + Topic + Payload */

    if (tlen == 0U || tlen > 127U || rlen > 127U) return;

    /* Byte 0: Fixed Header */
    SoftUART_SendByte(0x30U);

    /* Byte 1: Remaining Length */
    SoftUART_SendByte((uint8_t)rlen);

    /* Byte 2-3: Topic Length (MSB first) */
    SoftUART_SendByte((uint8_t)(tlen >> 8U));
    SoftUART_SendByte((uint8_t)(tlen & 0xFFU));

    /* Byte 4..: Topic */
    SoftUART_SendBuf((const uint8_t *)topic, tlen);

    /* Payload */
    SoftUART_SendBuf((const uint8_t *)payload, plen);
}
