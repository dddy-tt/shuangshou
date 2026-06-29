#include "vibrator.h"
#include "tim.h"

extern TIM_HandleTypeDef htim4;

/* ── 脉冲自动停止定时 ── */
static uint32_t pulse_end[2] = {0, 0};  /* 右手 / 左手 */

void Vibrator_Init(void)
{
    HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);  /* 右手 PD12 */
    HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_2);  /* 左手 PD13 */
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_2, 0);
}

void Vibrator_Set(uint8_t hand, uint8_t duty)
{
    if (duty > 100) duty = 100;
    uint32_t cmp = (uint32_t)duty * 10;  /* ARR=999, 占空比映射 */
    if (hand == VIB_RIGHT) {
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, cmp);
    } else {
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_2, cmp);
    }
}

void Vibrator_Pulse(uint8_t hand, uint16_t ms, uint8_t duty)
{
    Vibrator_Set(hand, duty);
    pulse_end[hand] = HAL_GetTick() + ms;
}

void Vibrator_Stop(uint8_t hand)
{
    if (hand == VIB_RIGHT) {
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0);
    } else {
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_2, 0);
    }
    pulse_end[hand] = 0;
}

/*
 * 周期性调用：检查并关闭到时的脉冲
 * 在 main.c while(1) 中每 50ms 调用一次
 */
void Vibrator_Tick(void)
{
    uint32_t now = HAL_GetTick();
    for (uint8_t h = 0; h < 2; h++) {
        if (pulse_end[h] != 0 && now >= pulse_end[h]) {
            Vibrator_Stop(h);
        }
    }
}
