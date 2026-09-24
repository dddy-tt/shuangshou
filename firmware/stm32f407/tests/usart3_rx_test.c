/* Host regression for the production USART3 RX recovery and command path. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define __MAIN_H
#define __USART_H__

typedef enum {
    HAL_OK = 0,
    HAL_ERROR = 1,
    HAL_BUSY = 2
} HAL_StatusTypeDef;

typedef struct {
    void *Instance;
    uint32_t ErrorCode;
    uint32_t RxState;
} UART_HandleTypeDef;

static int mock_usart3_instance;
#define USART3 ((void *)&mock_usart3_instance)
#define HAL_UART_ERROR_NONE 0x00U
#define HAL_UART_ERROR_PE   0x01U
#define HAL_UART_ERROR_NE   0x02U
#define HAL_UART_ERROR_FE   0x04U
#define HAL_UART_ERROR_ORE  0x08U
#define HAL_UART_STATE_READY 0x20U
#define HAL_UART_STATE_BUSY_RX 0x22U

static uint32_t mock_primask;
static uint8_t *mock_rx_destination;
static uint16_t mock_rx_size;
static uint32_t mock_rx_calls;
static uint32_t mock_clear_calls;
static uint32_t mock_rx_failures_remaining;
static uint32_t mock_tx_calls;

static uint32_t mock_get_primask(void) { return mock_primask; }
static void mock_disable_irq(void) { mock_primask = 1U; }
static void mock_restore_primask(uint32_t value) { mock_primask = value; }
static void mock_clear_uart_flags(UART_HandleTypeDef *huart)
{
    (void)huart;
    mock_clear_calls++;
}

#define __get_PRIMASK() mock_get_primask()
#define __disable_irq() mock_disable_irq()
#define __set_PRIMASK(value) mock_restore_primask(value)
#define __HAL_UART_CLEAR_PEFLAG(huart) mock_clear_uart_flags(huart)

int HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *data,
                        uint16_t size);
int HAL_UART_Transmit_IT(UART_HandleTypeDef *huart, uint8_t *data,
                         uint16_t size);

UART_HandleTypeDef huart3 = {USART3, HAL_UART_ERROR_NONE,
                             HAL_UART_STATE_READY};

/* Compile and exercise the production implementations directly. */
#include "../Core/Src/bluetooth.c"
#include "../Core/Src/usart3_rx.c"
#include "../Core/Src/test_input.c"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

int HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *data,
                        uint16_t size)
{
    mock_rx_calls++;
    if (huart != &huart3 || data == 0 || size != 1U) return HAL_ERROR;
    if (mock_rx_failures_remaining != 0U) {
        mock_rx_failures_remaining--;
        return HAL_ERROR;
    }
    if (huart->RxState != HAL_UART_STATE_READY) return HAL_BUSY;
    mock_rx_destination = data;
    mock_rx_size = size;
    huart->ErrorCode = HAL_UART_ERROR_NONE;
    huart->RxState = HAL_UART_STATE_BUSY_RX;
    return HAL_OK;
}

int HAL_UART_Transmit_IT(UART_HandleTypeDef *huart, uint8_t *data,
                         uint16_t size)
{
    (void)data;
    (void)size;
    mock_tx_calls++;
    return (huart == &huart3) ? HAL_OK : HAL_ERROR;
}

static void mock_receive_byte(uint8_t byte, uint32_t error_code)
{
    CHECK(mock_rx_destination != 0 && mock_rx_size == 1U,
          "one-byte receive is armed before injecting a byte");
    if (mock_rx_destination == 0 || mock_rx_size != 1U) return;
    *mock_rx_destination = byte;
    huart3.RxState = HAL_UART_STATE_READY; /* HAL completed one-byte transfer. */
    huart3.ErrorCode = error_code;
    HAL_UART_RxCpltCallback(&huart3);
}

static void mock_receive_text(const char *text)
{
    while (*text != '\0') {
        mock_receive_byte((uint8_t)*text, HAL_UART_ERROR_NONE);
        text++;
    }
}

static void mock_dispatch_one_test_command(uint32_t now_ms)
{
    char line[BT_RX_BUF_SIZE];
    char response[80];

    CHECK(BT_FetchLastString(line, sizeof(line)) != 0U,
          "complete TEST line is available to main-loop consumer");
    if (line[0] == '\0') return;
    CHECK(TestInput_HandleLine(line, now_ms, response, sizeof(response)) ==
          TEST_INPUT_HANDLED,
          "complete USART3 line reaches the production TEST parser");
    CHECK(strncmp(response, "[TEST] ", 7U) == 0,
          "production TEST parser returns its normal ACK");
}

static void test_end_to_end_test_enter_and_apply(void)
{
    static const char *const commands[] = {
        "TEST:ENTER\r\n",
        "TEST:FLEX|L1=0|L2=10|L3=20|L4=30|L5=40|R1=50|R2=60|R3=70|R4=80|R5=100\r\n",
        "TEST:IMU|R=10.00|P=-5.00|Y=2.00\r\n",
        "TEST:ACC|X=0.000|Y=0.000|Z=1.000|VALID=1\r\n",
        "TEST:APPLY\r\n"
    };
    const TestInput_Snapshot_t *snapshot;
    uint32_t i;

    BT_Init();
    TestInput_Init();
    huart3.RxState = HAL_UART_STATE_READY;
    CHECK(USART3_RxStart() == HAL_OK,
          "USART3 starts single-byte interrupt receive");

    for (i = 0U; i < sizeof(commands) / sizeof(commands[0]); i++) {
        mock_receive_text(commands[i]);
        mock_dispatch_one_test_command(i + 1U);
    }

    CHECK(TestInput_GetSource() == TEST_INPUT_SOURCE_VIRTUAL,
          "TEST:ENTER received through USART3 activates VIRTUAL mode");
    snapshot = TestInput_GetAppliedSnapshot();
    CHECK(snapshot != 0 && snapshot->flex[9] == 100U &&
          snapshot->angle[0] == 10.0f && snapshot->angle[1] == -5.0f &&
          snapshot->angle[2] == 2.0f && snapshot->acc[2] == 1.0f,
          "FLEX, IMU, ACC and APPLY reach the real parser through RX assembler");
}

static void test_error_drops_partial_but_preserves_completed_queue(void)
{
    char line[BT_RX_BUF_SIZE];
    char response[80];
    USART3_RxDiagnostics_t diag;
    BT_RxDiagnostics_t bt_diag;
    USART3_RxDiagnostics_t before;

    USART3_RxGetDiagnostics(&before);
    BT_Init();
    TestInput_Init();
    huart3.RxState = HAL_UART_STATE_READY;
    (void)USART3_RxStart();
    mock_receive_text("TEST:ENTER\n");
    mock_receive_text("TEST:IMU|R=1");

    /* HAL's ORE path has already stopped RX and restored READY here. */
    huart3.RxState = HAL_UART_STATE_READY;
    huart3.ErrorCode = HAL_UART_ERROR_ORE;
    HAL_UART_ErrorCallback(&huart3);
    CHECK(huart3.RxState == HAL_UART_STATE_BUSY_RX,
          "ORE error callback restarts HAL receive after HAL stopped it");
    CHECK(mock_clear_calls != 0U,
          "UART error path uses the STM32F4 SR/DR clear macro");

    CHECK(BT_FetchLastString(line, sizeof(line)) != 0U &&
          strcmp(line, "TEST:ENTER\n") == 0,
          "error reset preserves a complete line already queued");
    CHECK(TestInput_HandleLine(line, 10U, response, sizeof(response)) ==
          TEST_INPUT_HANDLED && TestInput_GetSource() ==
          TEST_INPUT_SOURCE_VIRTUAL,
          "preserved TEST:ENTER still reaches the production parser");

    mock_receive_text("U\n"); /* 丢掉 ORE 前一行的剩余尾部。 */
    mock_receive_text("TEST:EXIT\n");
    CHECK(BT_FetchLastString(line, sizeof(line)) != 0U &&
          strcmp(line, "TEST:EXIT\n") == 0,
          "partial pre-error bytes do not contaminate the next full command");
    CHECK(TestInput_HandleLine(line, 11U, response, sizeof(response)) ==
          TEST_INPUT_HANDLED && TestInput_GetSource() == TEST_INPUT_SOURCE_REAL,
          "clean post-recovery TEST:EXIT reaches the production parser");

    USART3_RxGetDiagnostics(&diag);
    BT_RxGetDiagnostics(&bt_diag);
    CHECK(diag.errors - before.errors == 1U &&
          diag.overrun_errors - before.overrun_errors == 1U &&
          diag.recoveries - before.recoveries == 1U,
          "ORE and successful automatic recovery are counted");
    CHECK(bt_diag.partial_resets == 1U,
          "UART error resets only the partial assembler once");
}

static void test_error_byte_and_nonblocking_error_recovery(void)
{
    USART3_RxDiagnostics_t diag;
    USART3_RxDiagnostics_t before;
    uint32_t calls_before;
    char line[BT_RX_BUF_SIZE];

    USART3_RxGetDiagnostics(&before);
    BT_Init();
    huart3.RxState = HAL_UART_STATE_READY;
    (void)USART3_RxStart();
    mock_receive_text("TEST:ENT");
    mock_receive_byte('X', HAL_UART_ERROR_FE);
    mock_receive_text("ER\n"); /* 丢掉带 FE 的 TEST:ENTER 行尾部。 */
    mock_receive_text("TEST:ENTER\n");
    CHECK(BT_FetchLastString(line, sizeof(line)) != 0U &&
          strcmp(line, "TEST:ENTER\n") == 0,
          "error-marked completion byte and preceding partial line are dropped");

    calls_before = mock_rx_calls;
    huart3.RxState = HAL_UART_STATE_BUSY_RX;
    huart3.ErrorCode = HAL_UART_ERROR_NE | HAL_UART_ERROR_PE;
    HAL_UART_ErrorCallback(&huart3);
    CHECK(mock_rx_calls == calls_before,
          "nonblocking FE/NE/PE does not redundantly restart BUSY_RX");
    CHECK(huart3.RxState == HAL_UART_STATE_BUSY_RX,
          "nonblocking error leaves HAL-owned active receive intact");

    USART3_RxGetDiagnostics(&diag);
    CHECK(diag.errors - before.errors == 2U &&
          diag.framing_errors - before.framing_errors == 1U &&
          diag.noise_errors - before.noise_errors == 1U &&
          diag.parity_errors - before.parity_errors == 1U,
          "FE, NE and PE diagnostics are counted by their HAL error bits");
}

static void test_failed_rearm_retries_without_spinning(void)
{
    uint32_t calls_before;
    USART3_RxDiagnostics_t diag;
    USART3_RxDiagnostics_t before;

    USART3_RxGetDiagnostics(&before);
    BT_Init();
    huart3.RxState = HAL_UART_STATE_READY;
    mock_rx_failures_remaining = 0U;
    (void)USART3_RxStart();

    mock_rx_failures_remaining = 1U;
    mock_receive_byte('x', HAL_UART_ERROR_NONE);
    calls_before = mock_rx_calls;
    USART3_RxService(0U);
    CHECK(mock_rx_calls == calls_before + 1U &&
          huart3.RxState == HAL_UART_STATE_BUSY_RX,
          "a failed callback rearm is retried by the main-loop service");

    mock_receive_byte('y', HAL_UART_ERROR_NONE);
    mock_rx_failures_remaining = 1U;
    mock_receive_byte('z', HAL_UART_ERROR_NONE);
    calls_before = mock_rx_calls;
    USART3_RxService(10U);
    CHECK(mock_rx_calls == calls_before,
          "pending recovery is rate-limited instead of spinning");
    USART3_RxService(20U);
    CHECK(mock_rx_calls == calls_before + 1U &&
          huart3.RxState == HAL_UART_STATE_BUSY_RX,
          "main-loop service retries at the configured interval");

    USART3_RxGetDiagnostics(&diag);
    CHECK(diag.rearm_failures - before.rearm_failures == 2U &&
          diag.recoveries - before.recoveries == 2U,
          "rearm failures and subsequent recoveries are observable");
}

static void test_overlong_line_lf_does_not_consume_next_line(void)
{
    uint16_t i;
    char line[BT_RX_BUF_SIZE];
    BT_RxDiagnostics_t diag;

    BT_Init();
    huart3.RxState = HAL_UART_STATE_READY;
    (void)USART3_RxStart();
    for (i = 0U; i < BT_RX_BUF_SIZE - 1U; i++) {
        mock_receive_byte('A', HAL_UART_ERROR_NONE);
    }
    mock_receive_byte('\n', HAL_UART_ERROR_NONE);
    mock_receive_text("TEST:ENTER\n");

    CHECK(BT_FetchLastString(line, sizeof(line)) != 0U &&
          strcmp(line, "TEST:ENTER\n") == 0,
          "LF at the overflow boundary drops only its overlong line");
    BT_RxGetDiagnostics(&diag);
    CHECK(diag.overlong_lines == 1U,
          "overflow-boundary line is counted once");
}

int main(void)
{
    test_end_to_end_test_enter_and_apply();
    test_error_drops_partial_but_preserves_completed_queue();
    test_error_byte_and_nonblocking_error_recovery();
    test_failed_rearm_retries_without_spinning();
    test_overlong_line_lf_does_not_consume_next_line();

    if (failures != 0) return 1;
    printf("usart3_rx_test: all checks passed (%lu TX calls)\n",
           (unsigned long)mock_tx_calls);
    return 0;
}
