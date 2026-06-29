#ifndef __VIBRATOR_H
#define __VIBRATOR_H

#include "stdint.h"

/* ── 手编号 ── */
#define VIB_RIGHT 0
#define VIB_LEFT  1

/* ── API ── */

/* 初始化：启动 TIM4 PWM 通道，占空比初始为 0 */
void Vibrator_Init(void);

/* 设置占空比 0~100（0=关，100=最大） */
void Vibrator_Set(uint8_t hand, uint8_t duty);

/* 短振一次（hand, 时长 ms, 占空比 0~100） */
void Vibrator_Pulse(uint8_t hand, uint16_t ms, uint8_t duty);

/* 立即停止 */
void Vibrator_Stop(uint8_t hand);

/* 周期性检查（自动关断到期脉冲），50ms 调用一次 */
void Vibrator_Tick(void);

#endif /* __VIBRATOR_H */
