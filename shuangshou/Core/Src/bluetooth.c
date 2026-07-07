#include "bluetooth.h"
#include "usart.h"
#include "string.h"

extern UART_HandleTypeDef huart3;

static uint8_t  rx_ring[BT_RX_BUF_SIZE];
static uint16_t rx_head = 0;
static uint16_t rx_tail = 0;

static uint8_t  cmd_pending = BT_CMD_NONE;

static char    str_buf[BT_RX_BUF_SIZE];
static uint8_t str_idx = 0;

static char    last_string[BT_RX_BUF_SIZE];
static uint8_t last_string_valid = 0;

void BT_Init(void)
{
    memset(rx_ring, 0, BT_RX_BUF_SIZE);
    memset(str_buf, 0, BT_RX_BUF_SIZE);
    memset(last_string, 0, BT_RX_BUF_SIZE);
    rx_head = 0;
    rx_tail = 0;
    str_idx = 0;
    cmd_pending = BT_CMD_NONE;
    last_string_valid = 0;
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

void BT_RxCallback(uint8_t byte)
{
    uint16_t next = (rx_head + 1) % BT_RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_ring[rx_head] = byte;
        rx_head = next;
    }

    if (byte >= '1' && byte <= '3') {
        cmd_pending = BT_CMD_SENS_1 + (byte - '1');
    }

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
            strncpy(last_string, str_buf, BT_RX_BUF_SIZE - 1U);
            last_string[BT_RX_BUF_SIZE - 1U] = '\0';
            last_string_valid = 1;
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

uint8_t BT_FetchLastString(char *out, uint16_t out_len)
{
    uint16_t copy_len;

    if (!out || out_len == 0U || !last_string_valid) {
        return 0U;
    }

    copy_len = (uint16_t)strlen(last_string);
    if (copy_len >= out_len) {
        copy_len = out_len - 1U;
    }

    memcpy(out, last_string, copy_len);
    out[copy_len] = '\0';

    last_string[0] = '\0';
    last_string_valid = 0;
    return 1U;
}
