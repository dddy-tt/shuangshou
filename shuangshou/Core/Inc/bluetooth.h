#ifndef __BLUETOOTH_H
#define __BLUETOOTH_H

#include "stdint.h"

/* ── 接收环形缓冲大小 ── */
#define BT_RX_BUF_SIZE  128

/* ── 蓝牙协议命令类型 ── */
#define BT_CMD_NONE     0
#define BT_CMD_CAL_MIN  1
#define BT_CMD_CAL_MAX  2
#define BT_CMD_SENS_1   3   /* 高灵敏度 */
#define BT_CMD_SENS_2   4   /* 中灵敏度 */
#define BT_CMD_SENS_3   5   /* 低灵敏度 */

void BT_Init(void);

/*
 * 发送字符串（阻塞，仅用于调试低频输出）
 */
void BT_SendString(const char *str);

/*
 * ★ v3.0: 发送原始二进制数据 (用于 18 字节遥测帧)
 * 不经过字符串转换, 直接 DMA 发送到 USART3
 */
void BT_SendRaw(const uint8_t *data, uint16_t len);

/*
 * 接收中断回调，由 HAL_UART_RxCpltCallback 调用
 */
void BT_RxCallback(uint8_t byte);

/*
 * 查询并消费一条完整指令
 * 返回: BT_CMD_xxx 或 BT_CMD_NONE
 */
uint8_t BT_GetCommand(void);

/*
 * 获取环缓冲中最近一条字符串命令（如 <CAL:MIN>）
 * 用于自定义手势录入等场景
 */
const char *BT_GetLastString(void);

#endif /* __BLUETOOTH_H */
