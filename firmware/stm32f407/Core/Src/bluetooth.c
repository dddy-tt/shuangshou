#include "bluetooth.h"
#include "usart.h"
#include "string.h"

extern UART_HandleTypeDef huart3;

/*
 * USART3 TX rings: 普通遥测和报警/消警分离。每次发送 API 调用会登记
 * 一个不可抢占的 TX 单元；单元内部仍可按 BT_TX_CHUNK_SIZE 分块，只有
 * 单元完整发送后才重新选择报警优先级。head 只由主循环发布，tail 只
 * 由 TX 完成中断发布。
 */
static uint8_t  tx_ring[BT_TX_BUF_SIZE];
static uint8_t  alarm_tx_ring[BT_TX_ALARM_BUF_SIZE];
static uint16_t tx_frame_lengths[BT_TX_BUF_SIZE];
static uint16_t alarm_tx_frame_lengths[BT_TX_ALARM_BUF_SIZE];
static volatile uint16_t tx_head = 0U;
static volatile uint16_t tx_tail = 0U;
static volatile uint16_t alarm_tx_head = 0U;
static volatile uint16_t alarm_tx_tail = 0U;
static volatile uint16_t tx_frame_head = 0U;
static volatile uint16_t tx_frame_tail = 0U;
static volatile uint16_t alarm_tx_frame_head = 0U;
static volatile uint16_t alarm_tx_frame_tail = 0U;
static volatile uint16_t tx_frame_remaining = 0U;
static volatile uint8_t  tx_frame_priority = 0U;
static volatile uint8_t  tx_frame_active = 0U;
static volatile uint16_t tx_inflight_len = 0U;
static volatile uint8_t  tx_inflight_priority = 0U;
static volatile uint8_t  tx_busy = 0U;

/* 接收 ISR 只发布完整行，主循环一次消费一行，避免 ACK 互相覆盖。 */
static char rx_line_queue[BT_RX_LINE_QUEUE_DEPTH][BT_RX_BUF_SIZE];
static volatile uint8_t rx_line_head = 0U;
static volatile uint8_t rx_line_tail = 0U;
static volatile uint8_t rx_line_count = 0U;

static volatile uint8_t cmd_pending = BT_CMD_NONE;

/* 字符串命令缓冲（只由接收 ISR 访问）。 */
static char    str_buf[BT_RX_BUF_SIZE];
static uint8_t str_idx = 0U;
static uint8_t rx_discard_until_lf = 0U;
static BT_RxDiagnostics_t rx_diagnostics;

/* BT_GetLastString 返回最近一次被主循环取出的完整行。 */
static char last_string[BT_RX_BUF_SIZE];

static uint32_t bt_irq_save(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void bt_irq_restore(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static uint16_t bt_tx_free(uint16_t head, uint16_t tail, uint16_t size)
{
    if (head >= tail) {
        return (uint16_t)(size - (head - tail) - 1U);
    }
    return (uint16_t)(tail - head - 1U);
}

static uint16_t bt_tx_advance(uint16_t index, uint16_t amount,
                              uint16_t size)
{
    uint32_t next = (uint32_t)index + amount;

    while (next >= size) next -= size;
    return (uint16_t)next;
}

static uint16_t bt_tx_next_index(uint16_t index, uint16_t size)
{
    return bt_tx_advance(index, 1U, size);
}

static uint16_t bt_tx_contiguous_count(uint16_t tail, uint16_t remaining,
                                       uint16_t size)
{
    uint16_t count = (uint16_t)(size - tail);

    if (count > remaining) count = remaining;
    if (count > BT_TX_CHUNK_SIZE) {
        count = BT_TX_CHUNK_SIZE;
    }
    return count;
}

static void bt_tx_kick(void)
{
    uint16_t count = 0U;
    uint8_t priority = 0U;
    uint16_t tail = 0U;
    uint32_t primask = bt_irq_save();

    /*
     * 只有当前 TX 单元完成后才择优。这样报警可以插到两个完整协议
     * 行之间，但不能插入一个 FLEX/BRINGUP 行的中间。
     */
    if (tx_busy == 0U) {
        if (tx_frame_active == 0U) {
            if (alarm_tx_frame_head != alarm_tx_frame_tail) {
                tx_frame_priority = 1U;
                tx_frame_remaining =
                    alarm_tx_frame_lengths[alarm_tx_frame_tail];
                tx_frame_active = 1U;
            } else if (tx_frame_head != tx_frame_tail) {
                tx_frame_priority = 0U;
                tx_frame_remaining = tx_frame_lengths[tx_frame_tail];
                tx_frame_active = 1U;
            }
        }

        if (tx_frame_active != 0U && tx_frame_remaining != 0U) {
            priority = tx_frame_priority;
            if (priority != 0U) {
                tail = alarm_tx_tail;
                count = bt_tx_contiguous_count(tail, tx_frame_remaining,
                                               BT_TX_ALARM_BUF_SIZE);
            } else {
                tail = tx_tail;
                count = bt_tx_contiguous_count(tail, tx_frame_remaining,
                                               BT_TX_BUF_SIZE);
            }
        }
    }

    if (count != 0U) {
        /* 先占住逻辑忙标志，再调用 HAL，防止主循环/完成 ISR 重入启动。 */
        tx_inflight_priority = priority;
        tx_inflight_len = count;
        tx_busy = 1U;

        if (priority != 0U) {
            if (HAL_UART_Transmit_IT(&huart3, &alarm_tx_ring[tail],
                                     count) != HAL_OK) {
                tx_busy = 0U;
                tx_inflight_len = 0U;
                tx_inflight_priority = 0U;
            }
        } else if (HAL_UART_Transmit_IT(&huart3, &tx_ring[tail], count) !=
                   HAL_OK) {
            tx_busy = 0U;
            tx_inflight_len = 0U;
            tx_inflight_priority = 0U;
        }
    }

    bt_irq_restore(primask);
}

static uint8_t bt_tx_enqueue(uint8_t *ring, volatile uint16_t *head,
                             volatile uint16_t *tail,
                             uint16_t *frame_lengths,
                             volatile uint16_t *frame_head,
                             volatile uint16_t *frame_tail,
                             uint16_t size, const uint8_t *data,
                             uint16_t len)
{
    uint16_t i;
    uint16_t write_head;
    uint16_t next_frame_head;
    uint16_t free_len;
    uint32_t primask;

    if (data == 0 || len == 0U || len >= size) return 0U;

    /* 禁止 TX 完成 ISR 在复制时推进游标；整帧写完后一次性发布 head。 */
    primask = bt_irq_save();
    free_len = bt_tx_free(*head, *tail, size);
    next_frame_head = bt_tx_next_index(*frame_head, size);
    if (len > free_len || next_frame_head == *frame_tail) {
        bt_irq_restore(primask);
        return 0U;
    }

    write_head = *head;
    for (i = 0U; i < len; i++) {
        ring[write_head] = data[i];
        write_head++;
        if (write_head >= size) write_head = 0U;
    }
    frame_lengths[*frame_head] = len;
    *head = write_head;
    *frame_head = next_frame_head;
    bt_irq_restore(primask);

    bt_tx_kick();
    return 1U;
}

void BT_Init(void)
{
    uint32_t primask = bt_irq_save();

    memset(rx_line_queue, 0, sizeof(rx_line_queue));
    memset(str_buf, 0, BT_RX_BUF_SIZE);
    memset(last_string, 0, BT_RX_BUF_SIZE);
    memset(&rx_diagnostics, 0, sizeof(rx_diagnostics));
    rx_line_head = 0U;
    rx_line_tail = 0U;
    rx_line_count = 0U;
    tx_head = 0U;
    tx_tail = 0U;
    alarm_tx_head = 0U;
    alarm_tx_tail = 0U;
    tx_frame_head = 0U;
    tx_frame_tail = 0U;
    alarm_tx_frame_head = 0U;
    alarm_tx_frame_tail = 0U;
    tx_frame_remaining = 0U;
    tx_frame_priority = 0U;
    tx_frame_active = 0U;
    tx_inflight_len = 0U;
    tx_inflight_priority = 0U;
    tx_busy = 0U;
    str_idx = 0U;
    rx_discard_until_lf = 0U;
    cmd_pending = BT_CMD_NONE;
    bt_irq_restore(primask);
}

uint8_t BT_SendString(const char *str)
{
    uint32_t len;

    if (str == 0) return 0U;
    len = (uint32_t)strlen(str);
    if (len > 0xFFFFUL) return 0U;
    return BT_SendRaw((const uint8_t *)str, (uint16_t)len);
}

uint8_t BT_SendRaw(const uint8_t *data, uint16_t len)
{
    return bt_tx_enqueue(tx_ring, &tx_head, &tx_tail, tx_frame_lengths,
                         &tx_frame_head, &tx_frame_tail, BT_TX_BUF_SIZE,
                         data, len);
}

uint8_t BT_SendAlarmString(const char *str)
{
    uint32_t len;

    if (str == 0) return 0U;
    len = (uint32_t)strlen(str);
    if (len > 0xFFFFUL) return 0U;
    return BT_SendAlarmRaw((const uint8_t *)str, (uint16_t)len);
}

uint8_t BT_SendAlarmRaw(const uint8_t *data, uint16_t len)
{
    return bt_tx_enqueue(alarm_tx_ring, &alarm_tx_head, &alarm_tx_tail,
                         alarm_tx_frame_lengths, &alarm_tx_frame_head,
                         &alarm_tx_frame_tail, BT_TX_ALARM_BUF_SIZE, data,
                         len);
}

void BT_TxService(void)
{
    bt_tx_kick();
}

void BT_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == 0 || huart->Instance != USART3) return;

    if (tx_busy != 0U && tx_inflight_len != 0U) {
        if (tx_inflight_priority != 0U) {
            alarm_tx_tail = bt_tx_advance(alarm_tx_tail, tx_inflight_len,
                                          BT_TX_ALARM_BUF_SIZE);
            tx_frame_remaining = (uint16_t)(tx_frame_remaining -
                                            tx_inflight_len);
        } else {
            tx_tail = bt_tx_advance(tx_tail, tx_inflight_len,
                                    BT_TX_BUF_SIZE);
            tx_frame_remaining = (uint16_t)(tx_frame_remaining -
                                            tx_inflight_len);
        }

        if (tx_frame_remaining == 0U) {
            if (tx_inflight_priority != 0U) {
                alarm_tx_frame_tail = bt_tx_next_index(
                    alarm_tx_frame_tail, BT_TX_ALARM_BUF_SIZE);
            } else {
                tx_frame_tail = bt_tx_next_index(tx_frame_tail,
                                                 BT_TX_BUF_SIZE);
            }
            tx_frame_priority = 0U;
            tx_frame_active = 0U;
        }
        tx_inflight_len = 0U;
        tx_inflight_priority = 0U;
        tx_busy = 0U;
    }
    bt_tx_kick();
}

static void bt_rx_publish_line(void)
{
    uint8_t next_head;

    /* RX ISR 不覆盖尚未消费的 ACK；队列满时丢弃整行，客户端会重发。 */
    if (rx_line_count >= BT_RX_LINE_QUEUE_DEPTH) {
        rx_diagnostics.lines_dropped++;
        return;
    }

    memcpy(rx_line_queue[rx_line_head], str_buf,
           (uint16_t)str_idx + 1U);
    next_head = (uint8_t)(rx_line_head + 1U);
    if (next_head >= BT_RX_LINE_QUEUE_DEPTH) next_head = 0U;
    rx_line_head = next_head;
    rx_line_count++;
}

/* ── 接收中断回调 ── */
void BT_RxCallback(uint8_t byte)
{
    if (rx_discard_until_lf != 0U) {
        if (byte == '\n') {
            rx_discard_until_lf = 0U;
            str_idx = 0U;
            str_buf[0] = '\0';
        }
        return;
    }

    /* 只把独立单字符命令识别为灵敏度切换；ALARM_ACK 等文本中的
       数字不能意外改动手势灵敏度。 */
    if (str_idx == 0U && byte >= '1' && byte <= '3') {
        cmd_pending = BT_CMD_SENS_1 + (byte - '1');
    }

    /* ── 字符串模式聚合 ── */
    if (str_idx < (BT_RX_BUF_SIZE - 1U)) {
        str_buf[str_idx++] = (char)byte;
    } else {
        rx_diagnostics.overlong_lines++;
        str_idx = 0U;
        str_buf[0] = '\0';
        /* 若溢出的当前字节本身就是 LF，本行已结束，不吞掉下一行。 */
        rx_discard_until_lf = (byte == '\n') ? 0U : 1U;
        return;
    }

    if (byte == '\n') {
        str_buf[str_idx] = '\0';
        rx_diagnostics.lines_received++;

        if (strstr(str_buf, "<CAL:MIN>")) {
            cmd_pending = BT_CMD_CAL_MIN;
        } else if (strstr(str_buf, "<CAL:MAX>")) {
            cmd_pending = BT_CMD_CAL_MAX;
        } else {
            bt_rx_publish_line();
        }
        str_idx = 0U;
        str_buf[0] = '\0';
    }
}

void BT_ResetRxAssembler(void)
{
    uint32_t primask = bt_irq_save();

    if (str_idx != 0U || rx_discard_until_lf != 0U) {
        rx_diagnostics.partial_resets++;
    }
    str_idx = 0U;
    str_buf[0] = '\0';
    /* 错误字节已被丢弃；继续丢弃其余尾部，直到重新同步到行边界。 */
    rx_discard_until_lf = 1U;
    bt_irq_restore(primask);
}

void BT_RxGetDiagnostics(BT_RxDiagnostics_t *out)
{
    uint32_t primask;

    if (out == 0) return;
    primask = bt_irq_save();
    *out = rx_diagnostics;
    bt_irq_restore(primask);
}

uint8_t BT_GetCommand(void)
{
    uint8_t cmd;
    uint32_t primask = bt_irq_save();

    cmd = cmd_pending;
    cmd_pending = BT_CMD_NONE;
    bt_irq_restore(primask);
    return cmd;
}

const char *BT_GetLastString(void)
{
    return last_string;
}

uint8_t BT_FetchLastString(char *out, uint16_t out_len)
{
    uint16_t copy_len;
    uint8_t tail;
    uint32_t primask;

    if (!out || out_len == 0U) {
        return 0U;
    }

    primask = bt_irq_save();
    if (rx_line_count == 0U) {
        bt_irq_restore(primask);
        return 0U;
    }

    tail = rx_line_tail;
    copy_len = (uint16_t)strlen(rx_line_queue[tail]);
    if (copy_len >= out_len) {
        copy_len = out_len - 1U;
    }

    memcpy(out, rx_line_queue[tail], copy_len);
    out[copy_len] = '\0';

    /* 保存稳定副本供旧 API 查询；队列槽位随后可由 ISR 重用。 */
    memcpy(last_string, rx_line_queue[tail],
           (uint16_t)strlen(rx_line_queue[tail]) + 1U);

    rx_line_tail = (uint8_t)(tail + 1U);
    if (rx_line_tail >= BT_RX_LINE_QUEUE_DEPTH) rx_line_tail = 0U;
    rx_line_count--;
    bt_irq_restore(primask);
    return 1U;
}
