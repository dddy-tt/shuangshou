/*
 * Host regression for the real Core/Src/bluetooth.c TX scheduler.
 *
 * The HAL and Cortex-M interrupt primitives below are deliberately tiny
 * test-local mocks.  HAL_UART_Transmit_IT() records the buffer handed to it;
 * mock_complete_tx() copies that transfer to the simulated wire and then
 * invokes the real completion callback.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Prevent the target headers from pulling in the STM32 device headers. */
#define __MAIN_H
#define __USART_H__

typedef struct {
    void *Instance;
} UART_HandleTypeDef;

static int mock_usart3_instance;
#define USART3 ((void *)&mock_usart3_instance)

#define HAL_OK    0
#define HAL_ERROR 1

static uint32_t mock_primask;

static uint32_t mock_get_primask(void)
{
    return mock_primask;
}

static void mock_disable_irq(void)
{
    mock_primask = 1U;
}

static void mock_set_primask(uint32_t primask)
{
    mock_primask = primask;
}

#define __get_PRIMASK()       mock_get_primask()
#define __disable_irq()       mock_disable_irq()
#define __set_PRIMASK(value)  mock_set_primask(value)

static const uint8_t *mock_pending_data;
static uint16_t mock_pending_len;
static uint32_t mock_hal_call_count;
static uint8_t mock_wire[32768];
static uint16_t mock_wire_len;

int HAL_UART_Transmit_IT(UART_HandleTypeDef *huart, uint8_t *data,
                         uint16_t len);

/* Include the production implementation, not a test double. */
#include "../Core/Src/bluetooth.c"

UART_HandleTypeDef huart3 = {USART3};

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("FAIL: %s\n", message);                            \
            failures++;                                                \
        }                                                              \
    } while (0)

int HAL_UART_Transmit_IT(UART_HandleTypeDef *huart, uint8_t *data,
                         uint16_t len)
{
    if (huart != &huart3 || huart->Instance != USART3 ||
        data == 0 || len == 0U || mock_pending_len != 0U) {
        return HAL_ERROR;
    }

    mock_pending_data = data;
    mock_pending_len = len;
    mock_hal_call_count++;
    return HAL_OK;
}

static void mock_reset_wire(void)
{
    mock_pending_data = 0;
    mock_pending_len = 0U;
    mock_hal_call_count = 0U;
    mock_wire_len = 0U;
}

static void mock_complete_tx(void)
{
    uint16_t len = mock_pending_len;

    CHECK(len != 0U, "completion requires a pending HAL transfer");
    if (len == 0U) return;

    CHECK((uint32_t)mock_wire_len + len <= sizeof(mock_wire),
          "simulated wire buffer is large enough");
    if ((uint32_t)mock_wire_len + len > sizeof(mock_wire)) return;

    memcpy(&mock_wire[mock_wire_len], mock_pending_data, len);
    mock_wire_len = (uint16_t)(mock_wire_len + len);
    mock_pending_data = 0;
    mock_pending_len = 0U;
    BT_TxCpltCallback(&huart3);
}

static void mock_drain_tx(void)
{
    uint32_t guard;

    for (guard = 0U; guard < 4096U; guard++) {
        if (mock_pending_len != 0U) {
            mock_complete_tx();
        } else {
            BT_TxService();
            if (mock_pending_len == 0U) return;
        }
    }

    CHECK(0, "TX queue drains without an infinite loop");
}

static uint16_t make_frame(uint8_t *frame, uint16_t len, const char *prefix)
{
    uint16_t i;
    uint16_t prefix_len = (uint16_t)strlen(prefix);

    CHECK(len > prefix_len + 2U, "test frame has room for a CRLF suffix");
    memcpy(frame, prefix, prefix_len);
    for (i = prefix_len; i + 2U < len; i++) {
        frame[i] = (uint8_t)('A' + (i % 26U));
    }
    frame[len - 2U] = '\r';
    frame[len - 1U] = '\n';
    return len;
}

static void check_wire_equals_two_frames(const uint8_t *first, uint16_t first_len,
                                         const uint8_t *second,
                                         uint16_t second_len,
                                         const char *message)
{
    CHECK(mock_wire_len == (uint16_t)(first_len + second_len), message);
    if (mock_wire_len != (uint16_t)(first_len + second_len)) return;
    CHECK(memcmp(mock_wire, first, first_len) == 0, message);
    CHECK(memcmp(&mock_wire[first_len], second, second_len) == 0, message);
}

static void test_long_frame_is_not_preempted(void)
{
    uint8_t flex[180];
    uint8_t alarm[31];
    uint16_t flex_len;
    uint16_t alarm_len;

    BT_Init();
    mock_reset_wire();
    flex_len = make_frame(flex, sizeof(flex), "FLEX|");
    alarm_len = make_frame(alarm, sizeof(alarm), "ALARM|");

    CHECK(BT_SendRaw(flex, flex_len) == 1U,
          "long FLEX frame is accepted as one TX item");
    CHECK(mock_pending_len == BT_TX_CHUNK_SIZE,
          "long frame starts with the configured chunk size");
    CHECK(BT_SendAlarmRaw(alarm, alarm_len) == 1U,
          "alarm is accepted while a normal frame is in flight");

    mock_drain_tx();
    check_wire_equals_two_frames(flex, flex_len, alarm, alarm_len,
                                 "alarm waits for the complete long FLEX frame");
}

static void test_ring_wrap_keeps_frame_and_alarm_order(void)
{
    uint8_t priming[1000];
    uint8_t bringup[180];
    uint8_t alarm[27];
    uint16_t bringup_len;
    uint16_t alarm_len;
    uint16_t i;

    BT_Init();
    mock_reset_wire();
    for (i = 0U; i < sizeof(priming); i++) {
        priming[i] = (uint8_t)(i & 0xFFU);
    }

    CHECK(BT_SendRaw(priming, sizeof(priming)) == 1U,
          "ring-priming item is accepted");
    mock_drain_tx();
    mock_reset_wire();

    bringup_len = make_frame(bringup, sizeof(bringup), "BRINGUP:");
    alarm_len = make_frame(alarm, sizeof(alarm), "ALARM|");
    CHECK(BT_SendRaw(bringup, bringup_len) == 1U,
          "wrapped BRINGUP frame is accepted");
    CHECK(mock_pending_len == (uint16_t)(BT_TX_BUF_SIZE - 1000U),
          "wrapped frame first transfer stops at the physical ring end");
    CHECK(BT_SendAlarmRaw(alarm, alarm_len) == 1U,
          "alarm is accepted during a wrapped frame");

    mock_drain_tx();
    check_wire_equals_two_frames(bringup, bringup_len, alarm, alarm_len,
                                 "ring wrap does not permit mid-frame alarm insertion");
}

static void test_full_queue_rejects_whole_item(void)
{
    uint8_t accepted[1000];
    uint8_t rejected[24];
    uint16_t i;

    BT_Init();
    mock_reset_wire();
    for (i = 0U; i < sizeof(accepted); i++) {
        accepted[i] = (uint8_t)(0x80U + (i & 0x7FU));
    }
    memset(rejected, 0xEE, sizeof(rejected));

    CHECK(BT_SendRaw(accepted, sizeof(accepted)) == 1U,
          "accepted item fills almost the entire normal ring");
    CHECK(BT_SendRaw(rejected, sizeof(rejected)) == 0U,
          "item larger than remaining ring space is rejected atomically");
    mock_drain_tx();

    CHECK(mock_wire_len == sizeof(accepted),
          "rejected item contributes no bytes to the wire");
    CHECK(memcmp(mock_wire, accepted, sizeof(accepted)) == 0,
          "accepted item remains byte-for-byte intact after rejection");
}

int main(void)
{
    test_long_frame_is_not_preempted();
    test_ring_wrap_keeps_frame_and_alarm_order();
    test_full_queue_rejects_whole_item();

    if (failures != 0) {
        printf("bluetooth_tx_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("bluetooth_tx_test: all checks passed (%lu HAL chunks)\n",
           (unsigned long)mock_hal_call_count);
    return 0;
}
