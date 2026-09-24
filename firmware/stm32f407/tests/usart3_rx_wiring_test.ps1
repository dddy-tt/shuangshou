$ErrorActionPreference = 'Stop'

$firmwareRoot = Join-Path $PSScriptRoot '..'
$main = Get-Content -Raw (Join-Path $firmwareRoot 'Core\Src\main.c')
$rx = Get-Content -Raw (Join-Path $firmwareRoot 'Core\Src\usart3_rx.c')
$bluetooth = Get-Content -Raw (Join-Path $firmwareRoot 'Core\Src\bluetooth.c')
$project = Get-Content -Raw (Join-Path $firmwareRoot 'MDK-ARM\shuangshou.uvprojx')
$hal = Get-Content -Raw (Join-Path $firmwareRoot 'Drivers\STM32F4xx_HAL_Driver\Src\stm32f4xx_hal_uart.c')
$protocol = Get-Content -Raw (Join-Path $firmwareRoot '..\..\docs\protocol.md')

if ($main -match 'HAL_UART_RxCpltCallback|uart3_rx_byte|HAL_UART_Receive_IT') {
    throw 'USART3 RX callback and HAL rearm must be owned by usart3_rx.c.'
}
if ($main -notmatch 'USART3_RxStart\(\)' -or
    $main -notmatch 'USART3_RxService\(now\)') {
    throw 'main.c must start RX and service deferred receive recovery.'
}
if ($rx -notmatch 'HAL_UART_RxCpltCallback' -or
    $rx -notmatch 'HAL_UART_ErrorCallback' -or
    $rx -notmatch 'huart->Instance != USART3') {
    throw 'USART3-owned HAL receive and error callbacks are incomplete.'
}
foreach ($errorBit in @('HAL_UART_ERROR_ORE', 'HAL_UART_ERROR_FE',
                        'HAL_UART_ERROR_NE', 'HAL_UART_ERROR_PE')) {
    if ($rx -notmatch [regex]::Escape($errorBit)) {
        throw "Missing HAL RX error accounting for $errorBit."
    }
}
if ($rx -notmatch 'BT_ResetRxAssembler\(\)' -or
    $rx -notmatch '__HAL_UART_CLEAR_PEFLAG\(huart\)' -or
    $rx -notmatch 'HAL_UART_Receive_IT\(&huart3') {
    throw 'UART error handling must reset only the partial line, clear F4 flags, and rearm HAL RX.'
}
if ($bluetooth -notmatch 'rx_diagnostics\.lines_dropped\+\+' -or
    $bluetooth -notmatch 'void BT_ResetRxAssembler\(') {
    throw 'RX assembler must expose partial resets and completed-line queue drops.'
}
if ($main -notmatch 'TestInput_MainQueueAck\(test_response\)' -or
    $main -notmatch 'BT_SendString\(test_ack_queue\[test_ack_tail\]\) == 0U' -or
    $main -notmatch 'test_ack_retry_count\+\+') {
    throw 'TEST ACKs must remain pending when the normal TX queue is full.'
}
if ($main -match 'BT_SendAlarmString\(test_ack_queue') {
    throw 'TEST ACK retry must not use or outrank the alarm TX queue.'
}
if ($main -notmatch 'UART3_RX\|BYTES=%lu\|LINES=%lu' -or
    $protocol -notmatch 'UART3_RX\|BYTES=') {
    throw 'The low-rate UART RX diagnostic frame must be emitted and documented.'
}
if ($project -notmatch '<FileName>usart3_rx\.c</FileName>') {
    throw 'Keil project does not compile usart3_rx.c.'
}
if ($hal -notmatch 'HAL_UART_ERROR_ORE' -or
    $hal -notmatch 'UART_EndRxTransfer\(huart\)' -or
    $hal -notmatch 'HAL_UART_ErrorCallback\(huart\)') {
    throw 'Bundled STM32F4 HAL ORE stop-and-error-callback behavior was not found.'
}

Write-Output 'usart3_rx_wiring_test: all checks passed'
