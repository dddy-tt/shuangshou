#include "usart3_rx.h"

#include "usart.h"
#include "bluetooth.h"

#define USART3_RX_RETRY_INTERVAL_MS 20U

static volatile uint8_t usart3_rx_byte;
static volatile uint8_t usart3_rx_retry_pending;
static uint32_t usart3_rx_last_retry_ms;
static uint8_t usart3_rx_retry_seen;
static volatile USART3_RxDiagnostics_t usart3_rx_diagnostics;

static uint8_t usart3_rx_arm(void)
{
    if (HAL_UART_Receive_IT(&huart3, (uint8_t *)&usart3_rx_byte, 1U) ==
        HAL_OK) {
        usart3_rx_retry_pending = 0U;
        return 1U;
    }

    usart3_rx_diagnostics.rearm_failures++;
    usart3_rx_retry_pending = 1U;
    return 0U;
}

static void usart3_rx_record_error(uint32_t error_code)
{
    usart3_rx_diagnostics.errors++;
    if ((error_code & HAL_UART_ERROR_ORE) != 0U) {
        usart3_rx_diagnostics.overrun_errors++;
    }
    if ((error_code & HAL_UART_ERROR_FE) != 0U) {
        usart3_rx_diagnostics.framing_errors++;
    }
    if ((error_code & HAL_UART_ERROR_NE) != 0U) {
        usart3_rx_diagnostics.noise_errors++;
    }
    if ((error_code & HAL_UART_ERROR_PE) != 0U) {
        usart3_rx_diagnostics.parity_errors++;
    }
}

HAL_StatusTypeDef USART3_RxStart(void)
{
    HAL_StatusTypeDef status;

    usart3_rx_retry_pending = 0U;
    usart3_rx_retry_seen = 0U;
    status = HAL_UART_Receive_IT(&huart3, (uint8_t *)&usart3_rx_byte, 1U);
    if (status == HAL_OK) return HAL_OK;

    usart3_rx_diagnostics.rearm_failures++;
    usart3_rx_retry_pending = 1U;
    return status;
}

void USART3_RxService(uint32_t now_ms)
{
    if (usart3_rx_retry_pending == 0U) return;
    if (usart3_rx_retry_seen != 0U &&
        (uint32_t)(now_ms - usart3_rx_last_retry_ms) <
        USART3_RX_RETRY_INTERVAL_MS) {
        return;
    }

    usart3_rx_retry_seen = 1U;
    usart3_rx_last_retry_ms = now_ms;
    if (huart3.RxState == HAL_UART_STATE_READY && usart3_rx_arm() != 0U) {
        usart3_rx_diagnostics.recoveries++;
    }
}

void USART3_RxGetDiagnostics(USART3_RxDiagnostics_t *out)
{
    uint32_t primask;

    if (out == 0) return;
    primask = __get_PRIMASK();
    __disable_irq();
    out->bytes_received = usart3_rx_diagnostics.bytes_received;
    out->errors = usart3_rx_diagnostics.errors;
    out->overrun_errors = usart3_rx_diagnostics.overrun_errors;
    out->framing_errors = usart3_rx_diagnostics.framing_errors;
    out->noise_errors = usart3_rx_diagnostics.noise_errors;
    out->parity_errors = usart3_rx_diagnostics.parity_errors;
    out->recoveries = usart3_rx_diagnostics.recoveries;
    out->rearm_failures = usart3_rx_diagnostics.rearm_failures;
    __set_PRIMASK(primask);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uint32_t error_code;
    uint8_t byte;

    if (huart == 0 || huart->Instance != USART3) return;

    byte = usart3_rx_byte;
    usart3_rx_diagnostics.bytes_received++;
    error_code = huart->ErrorCode;

    if (error_code != HAL_UART_ERROR_NONE) {
        usart3_rx_record_error(error_code);
        BT_ResetRxAssembler();
        __HAL_UART_CLEAR_PEFLAG(huart);
        if (usart3_rx_arm() != 0U) {
            usart3_rx_diagnostics.recoveries++;
        }
        return; /* 含 UART 错误的字节不进入协议组帧器。 */
    }

    /* 先重挂接，保持单字节接收窗口尽可能短；再消费已收字节。 */
    (void)usart3_rx_arm();
    BT_RxCallback(byte);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    uint32_t error_code;

    if (huart == 0 || huart->Instance != USART3) return;
    error_code = huart->ErrorCode;
    if (error_code == HAL_UART_ERROR_NONE) return;

    usart3_rx_record_error(error_code);
    BT_ResetRxAssembler();
    /* STM32F4 HAL 要求按 SR 后 DR 读序列清除 PE/FE/NE/ORE。 */
    __HAL_UART_CLEAR_PEFLAG(huart);

    /* HAL 对 ORE 已结束当前 RX 并置 READY；FE/NE/PE 通常仍在 BUSY_RX，
       由 HAL 继续原接收，不 abort、不重复挂接。 */
    if (huart->RxState == HAL_UART_STATE_READY) {
        if (usart3_rx_arm() != 0U) {
            usart3_rx_diagnostics.recoveries++;
        }
    }
}
