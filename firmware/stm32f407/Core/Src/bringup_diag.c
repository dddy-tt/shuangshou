#include "bringup_diag.h"

#include "bluetooth.h"
#include "stdio.h"
#include "string.h"

static BringupDiag_State_t g_bringup_diag;
static char g_bringup_line[192];

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

void BringupDiag_SetMAX30102PartId(uint8_t part_id)
{
    g_bringup_diag.max_part_id = part_id;
}

void BringupDiag_SetMAX30102HalErr(uint8_t hal_err)
{
    g_bringup_diag.max_hal_err = hal_err;
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

uint8_t BringupDiag_TrySend(void)
{
    int len;

    /* JY/JY_RET 使用当前报警输入的右手 JY61P；其余字段保留 MAX 诊断。
       不输出 JY_R/JY_L 复合字段，避免旧客户端用未启用左手覆盖 JY 聚合值。 */
    len = snprintf(g_bringup_line, sizeof(g_bringup_line),
                   "BRINGUP: JY=%u,JY_RET=%u,ADC1=%u,ADC2=%u,BEEP=1,"
                   "MAX=%u,MAX_RET=%u,MAX_PART=0x%02X,MAX_HAL_ERR=%d,DEG=%u\r\n",
                   g_bringup_diag.jy_right_ok,
                   g_bringup_diag.jy_right_ret,
                   g_bringup_diag.adc1_seen,
                   g_bringup_diag.adc2_seen,
                   g_bringup_diag.max_ok,
                   g_bringup_diag.max_ret,
                   g_bringup_diag.max_part_id,
                   (int8_t)g_bringup_diag.max_hal_err,
                   g_bringup_diag.degraded);
    if (len <= 0 || len >= (int)sizeof(g_bringup_line)) {
        return 0U;
    }

    /* 不直接调用 HAL_UART_Transmit_IT，避免和 USART3 TX 队列竞争。 */
    return BT_SendString(g_bringup_line);
}
