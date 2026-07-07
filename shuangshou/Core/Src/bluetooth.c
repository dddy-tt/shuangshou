#include "bluetooth.h"
#include "usart.h"
#include "string.h"

extern UART_HandleTypeDef huart3;

static uint8_t  rx_ring[BT_RX_BUF_SIZE];
static uint16_t rx_head = 0;
static uint16_t rx_tail = 0;

static uint8_t  cmd_pending = BT_CMD_NONE;

/* 字符串命令缓冲 */
static char    str_buf[BT_RX_BUF_SIZE];
static uint8_t str_idx = 0;

static char    last_string[BT_RX_BUF_SIZE];

void BT_Init(void)
{
    memset(rx_ring, 0, BT_RX_BUF_SIZE);
    memset(str_buf, 0, BT_RX_BUF_SIZE);
    memset(last_string, 0, BT_RX_BUF_SIZE);
    rx_head = 0;
    rx_tail = 0;
    str_idx = 0;
    cmd_pending = BT_CMD_NONE;
}

void BT_SendString(const char *str)
{
    if (!str) return;
    HAL_UART_Transmit(&huart3, (uint8_t *)str, strlen(str), 100);
}

void BT_SendRaw(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0U) return;
    HAL_UART_Transmit(&huart3, (uint8_t *)data, len, 100);
}

/* ── 接收中断回调 ── */
void BT_RxCallback(uint8_t byte)
{
    /* 存入环形缓冲 */
    uint16_t next = (rx_head + 1) % BT_RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_ring[rx_head] = byte;
        rx_head = next;
    }

    /* ── 单字符指令（灵敏度控制） ── */
    if (byte >= '1' && byte <= '3') {
        cmd_pending = BT_CMD_SENS_1 + (byte - '1');
    }

    /* ── 字符串模式聚合 ── */
    if (str_idx < (BT_RX_BUF_SIZE - 1U)) {
        str_buf[str_idx++] = (char)byte;
    } else {
        str_buf[BT_RX_BUF_SIZE - 2U] = (char)byte;
        str_idx = BT_RX_BUF_SIZE - 1U;
    }

    if (byte == '\n' || str_idx >= (BT_RX_BUF_SIZE - 1U)) {
        str_buf[str_idx] = '\0';
        str_idx = 0;

        if (strstr(str_buf, "<CAL:MIN>")) {
            cmd_pending = BT_CMD_CAL_MIN;
        } else if (strstr(str_buf, "<CAL:MAX>")) {
            cmd_pending = BT_CMD_CAL_MAX;
        } else {
            /* 非标定指令，保存为自定义字符串 */
            strncpy(last_string, str_buf, BT_RX_BUF_SIZE - 1U);
            last_string[BT_RX_BUF_SIZE - 1U] = '\0';
        }
        memset(str_buf, 0, BT_RX_BUF_SIZE);
    }
}

uint8_t BT_GetCommand(void)
{
    uint8_t cmd = cmd_pending;
    cmd_pending = BT_CMD_NONE;
    return cmd;
}

const char *BT_GetLastString(void)
{
    return last_string;
}
