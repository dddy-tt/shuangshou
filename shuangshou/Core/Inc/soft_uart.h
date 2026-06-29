/**
  ******************************************************************************
  * @file           : soft_uart.h
  * @brief          : 软件模拟 UART TX-only 驱动 (PE0, 9600bps)
  * @author         : v3.0 IoT 控制子系统
  * @date           : 2026-06-29
  *
  * @用途:
  *   F407 PE0 → ESP-01S RX, 发 MQTT PUBLISH 帧控制继电器
  *   不占用硬件 UART, 不修改 CubeMX
  *
  * @时序:
  *   9600bps → 每 bit 104.17μs
  *   DWT 周期计数器 (CYCCNT) @ 168MHz → 1 cycle = 5.95ns
  *   104.17μs = 17500 cycles
  ******************************************************************************
  */

#ifndef __SOFT_UART_H
#define __SOFT_UART_H

#include "stdint.h"

/* ── 引脚定义 ── */
#define SOFTUART_TX_PORT   GPIOE
#define SOFTUART_TX_PIN    GPIO_PIN_0
#define SOFTUART_BAUD      9600U

/* ── API ── */

/**
 * @brief  初始化软串口 (PE0 推挽输出 + DWT 周期计数器)
 * @note   在 main() 初始化阶段调用, SoftI2C_Init() 之后任意位置
 */
void SoftUART_Init(void);

/**
 * @brief  发送单个字节 (阻塞, ~1.04ms @ 9600bps)
 * @note   仅在 50ms/100ms 低频任务中调用, 不阻塞实时性
 */
void SoftUART_SendByte(uint8_t byte);

/**
 * @brief  发送字节数组
 */
void SoftUART_SendBuf(const uint8_t *buf, uint16_t len);

/**
 * @brief  构造并发送 MQTT PUBLISH 帧 (QoS 0)
 * @param  topic    MQTT Topic 字符串 (如 "glove/light")
 * @param  payload  MQTT Payload 字符串 (如 "ON")
 * @note   帧格式: [0x30][RemainingLen][TLenH][TLenL][Topic...][Payload...]
 *         无 Message ID (QoS 0), 无 Retain 标志
 */
void SoftUART_SendMQTT(const char *topic, const char *payload);

#endif /* __SOFT_UART_H */
