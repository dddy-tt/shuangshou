#ifndef USART3_RX_H
#define USART3_RX_H

#include "main.h"

typedef struct {
    uint32_t bytes_received;
    uint32_t errors;
    uint32_t overrun_errors;
    uint32_t framing_errors;
    uint32_t noise_errors;
    uint32_t parity_errors;
    uint32_t recoveries;
    uint32_t rearm_failures;
} USART3_RxDiagnostics_t;

/* 启动 USART3 单字节 IT 接收；失败会登记为待重试。 */
HAL_StatusTypeDef USART3_RxStart(void);

/* 主循环调用；接收重挂接失败时按低频间隔尝试恢复。 */
void USART3_RxService(uint32_t now_ms);
void USART3_RxGetDiagnostics(USART3_RxDiagnostics_t *out);

#endif /* USART3_RX_H */
