#include "bringup_diag.h"

#include "stdio.h"
#include "string.h"

static BringupDiag_State_t g_bringup_diag;
static char g_bringup_line[112];

void BringupDiag_Init(void)
{
    memset(&g_bringup_diag, 0, sizeof(g_bringup_diag));
    memset(g_bringup_line, 0, sizeof(g_bringup_line));
}

void BringupDiag_SetJYRightResult(uint8_t ok, uint8_t ret)
{
    g_bringup_diag.jy_right_ok = ok ? 1U : 0U;
    g_bringup_diag.jy_right_ret = ret;
}

void BringupDiag_SetJYLeftResult(uint8_t ok, uint8_t ret)
{
    g_bringup_diag.jy_left_ok = ok ? 1U : 0U;
    g_bringup_diag.jy_left_ret = ret;
}

void BringupDiag_SetMAX30102Result(uint8_t ok, uint8_t ret)
{
    g_bringup_diag.max_ok = ok ? 1U : 0U;
    g_bringup_diag.max_ret = ret;
}

void BringupDiag_SetADCSeen(uint8_t adc1_seen, uint8_t adc2_seen)
{
    g_bringup_diag.adc1_seen = adc1_seen ? 1U : 0U;
    g_bringup_diag.adc2_seen = adc2_seen ? 1U : 0U;
}

void BringupDiag_RecomputeDegraded(void)
{
    g_bringup_diag.degraded =
        (g_bringup_diag.jy_right_ok &&
         g_bringup_diag.jy_left_ok &&
         g_bringup_diag.max_ok) ? 0U : 1U;
}

const BringupDiag_State_t *BringupDiag_GetState(void)
{
    return &g_bringup_diag;
}

uint8_t BringupDiag_TrySend(UART_HandleTypeDef *huart)
{
    int len;

    if (huart == NULL) {
        return 0U;
    }

    if (huart->gState != HAL_UART_STATE_READY) {
        return 0U;
    }

    len = snprintf(g_bringup_line, sizeof(g_bringup_line),
                   "BRINGUP: JY_R=%u,JY_L=%u,JY_R_RET=%u,JY_L_RET=%u,MAX=%u,MAX_RET=%u,ADC1=%u,ADC2=%u,DEG=%u\r\n",
                   g_bringup_diag.jy_right_ok,
                   g_bringup_diag.jy_left_ok,
                   g_bringup_diag.jy_right_ret,
                   g_bringup_diag.jy_left_ret,
                   g_bringup_diag.max_ok,
                   g_bringup_diag.max_ret,
                   g_bringup_diag.adc1_seen,
                   g_bringup_diag.adc2_seen,
                   g_bringup_diag.degraded);
    if (len <= 0) {
        return 0U;
    }

    if (HAL_UART_Transmit_IT(huart, (uint8_t *)g_bringup_line, (uint16_t)len) != HAL_OK) {
        return 0U;
    }

    return 1U;
}
