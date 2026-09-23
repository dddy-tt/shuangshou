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
    uint8_t max_part_id;
    uint8_t max_hal_err;        /* HAL error code from last failed I2C op */
    uint8_t adc1_seen;
    uint8_t adc2_seen;
    uint8_t degraded;
} BringupDiag_State_t;

void BringupDiag_Init(void);
void BringupDiag_SetJYRightResult(uint8_t ok, uint8_t ret);
void BringupDiag_SetJYLeftResult(uint8_t ok, uint8_t ret);
void BringupDiag_SetMAX30102Result(uint8_t ok, uint8_t ret);
void BringupDiag_SetMAX30102PartId(uint8_t part_id);
void BringupDiag_SetMAX30102HalErr(uint8_t hal_err);
void BringupDiag_SetADCSeen(uint8_t adc1_seen, uint8_t adc2_seen);
void BringupDiag_RecomputeDegraded(void);
const BringupDiag_State_t *BringupDiag_GetState(void);
/* 通过 USART3 蓝牙统一队列发送，返回 1 表示整帧已入普通队列。 */
uint8_t BringupDiag_TrySend(void);

#endif /* __BRINGUP_DIAG_H */
