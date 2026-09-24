#ifndef __BLUETOOTH_H
#define __BLUETOOTH_H

#include "main.h"

/* ── 接收环形缓冲大小 ── */
#define BT_RX_BUF_SIZE  128
#define BT_RX_LINE_QUEUE_DEPTH 4U
#define BT_TX_BUF_SIZE  1024U
#define BT_TX_ALARM_BUF_SIZE 256U
#define BT_TX_CHUNK_SIZE 64U

/* ── 蓝牙协议命令类型 ── */
#define BT_CMD_NONE     0
#define BT_CMD_CAL_MIN  1
#define BT_CMD_CAL_MAX  2
#define BT_CMD_SENS_1   3   /* 高灵敏度 */
#define BT_CMD_SENS_2   4   /* 中灵敏度 */
#define BT_CMD_SENS_3   5   /* 低灵敏度 */

typedef struct {
    uint32_t lines_received;
    uint32_t lines_dropped;
    uint32_t partial_resets;
    uint32_t overlong_lines;
} BT_RxDiagnostics_t;

void BT_Init(void);

/*
 * 发送字符串（复制到 USART3 非阻塞发送队列）。一次调用是一个不可
 * 抢占的 TX 单元；协议文本调用者应一次传入完整的 CRLF 行。
 */
/* 返回 1 表示整帧已复制到队列，0 表示参数非法或队列空间不足。 */
uint8_t BT_SendString(const char *str);

/*
 * 发送原始数据（复制到 USART3 非阻塞发送队列）。保留原始字节契约；
 * 一次调用的字节不会与另一优先级交错。
 */
uint8_t BT_SendRaw(const uint8_t *data, uint16_t len);

/* 报警/消警使用独立高优先级队列；同样只报告“已入队”状态。 */
uint8_t BT_SendAlarmString(const char *str);
uint8_t BT_SendAlarmRaw(const uint8_t *data, uint16_t len);

/* 主循环轮询一次发送队列；不等待 UART。 */
void BT_TxService(void);

/* 由 HAL_UART_TxCpltCallback 转发，推进队列中的下一段数据。 */
void BT_TxCpltCallback(UART_HandleTypeDef *huart);

/*
 * 接收中断回调，由 HAL_UART_RxCpltCallback 调用
 */
void BT_RxCallback(uint8_t byte);

/*
 * 查询并消费一条完整指令
 * 返回: BT_CMD_xxx 或 BT_CMD_NONE
 */
uint8_t BT_GetCommand(void);

/* 获取最近一次由 BT_FetchLastString 取出的完整字符串快照。 */
const char *BT_GetLastString(void);

/*
 * 消费接收队列中的最早一条完整字符串命令
 * 返回 1 = 成功取到，0 = 当前没有新字符串命令
 */
uint8_t BT_FetchLastString(char *out, uint16_t out_len);

/* UART 错误后丢弃当前受损行至 LF；已入队完整行及全部 TX 状态保留。 */
void BT_ResetRxAssembler(void);
void BT_RxGetDiagnostics(BT_RxDiagnostics_t *out);

#endif /* __BLUETOOTH_H */
