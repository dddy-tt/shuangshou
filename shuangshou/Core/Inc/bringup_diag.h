#ifndef __BRINGUP_DIAG_H
#define __BRINGUP_DIAG_H

#include "main.h"

typedef struct {
    uint8_t jy_right_ok;
    uint8_t jy_left_ok;
    uint8_t jy_right_ret;
    uint8_t jy_left_ret;
    uint8_t max_ok;
    uint8_t max_ret;
    uint8_t adc1_seen;
    uint8_t adc2_seen;
    uint8_t degraded;
} BringupDiag_State_t;

void BringupDiag_Init(void);
void BringupDiag_SetJYRightResult(uint8_t ok, uint8_t ret);
void BringupDiag_SetJYLeftResult(uint8_t ok, uint8_t ret);
void BringupDiag_SetMAX30102Result(uint8_t ok, uint8_t ret);
void BringupDiag_SetADCSeen(uint8_t adc1_seen, uint8_t adc2_seen);
void BringupDiag_RecomputeDegraded(void);
const BringupDiag_State_t *BringupDiag_GetState(void);
uint8_t BringupDiag_TrySend(UART_HandleTypeDef *huart);

#endif /* __BRINGUP_DIAG_H */
