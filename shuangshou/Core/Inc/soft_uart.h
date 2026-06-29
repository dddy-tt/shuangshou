/**
  ******************************************************************************
  * @file           : soft_uart.h
  * @brief          : 软件模拟 UART TX-only — 115200bps DWT 精确定时
  * @author         : v3.0 → v3.1 Gemini 评审修正
  * @date           : 2026-06-29
  *
  * @用途:
  *   F407 PE0 → ESP-01S RX, 发送 AT+MQTTPUB 指令控制 IoT 设备
  *   115200bps 波特率确保单次命令阻塞 < 4ms, 不击穿 5ms 轮询槽
  *
  * @修复记录 (v3.1):
  *   盲点 2: 二进制 MQTT 裸帧 → ASCII AT+MQTTPUB 命令
  *   盲点 3: 9600bps → 115200bps (DWT 1458 cycles/bit)
  *          发送全程关总中断保护波形, 阻塞 < 4ms
  ******************************************************************************
  */

#ifndef __SOFT_UART_H
#define __SOFT_UART_H

#include "stdint.h"

/* ── 引脚 ── */
#define SOFTUART_TX_PORT   GPIOE
#define SOFTUART_TX_PIN    GPIO_PIN_0

/* ── API ── */

/**
 * @brief 初始化软串口 (PE0 推挽输出 + DWT 周期计数器)
 */
void SoftUART_Init(void);

/**
 * @brief  发送 AT+MQTTPUB 命令到 ESP-01S
 * @param  topic   MQTT Topic 字符串
 * @param  payload MQTT Payload 字符串
 * @note   格式: AT+MQTTPUB=0,"topic","payload",0,0\r\n
 *         @115200bps, ~45 字节, 阻塞约 3.9ms
 *         全程关总中断保护波形完整
 */
void SoftUART_SendMQTT(const char *topic, const char *payload);

#endif /* __SOFT_UART_H */
