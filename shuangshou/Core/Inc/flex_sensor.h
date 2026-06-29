/**
  ******************************************************************************
  * @file           : flex_sensor.h
  * @brief          : 柔性传感器驱动 — Ping-Pong 双缓冲快照 v2.4
  * @author         : 车架师重构版 v2.4
  * @date           : 2026-06-29
  *
  * @核心升级 (v2.3 → v2.4):
  *   ★ 无锁 Ping-Pong 双缓冲: ADC DMA 缓冲区扩容为 5chn×2=10 半字,
  *      HalfCplt 中断锁存前半组, Cplt 中断锁存后半组,
  *      上层 Flex_Update 始终读取已完成的那一半,
  *      彻底删除 HAL_ADC_Stop_DMA/Start_DMA 高频切流。
  ******************************************************************************
  */

#ifndef __FLEX_SENSOR_H
#define __FLEX_SENSOR_H

#include "stdint.h"

#define HISTORY_SIZE 10
#define FINGER_NUM   5

/* ── Ping-Pong 缓冲区大小: 5 通道 × 2 组 ── */
#define ADC_PINGPONG_SIZE  (FINGER_NUM * 2U)  /* = 10 */

/* ── 手指索引 ── */
typedef enum {
    THUMB = 0, INDEX, MIDDLE, RING, PINKY, FINGER_COUNT = 5
} Finger_t;

/* ── 单指数据结构 ── */
typedef struct {
    uint16_t current_raw;
    uint8_t  pos_percent;
    uint8_t  history_buffer[HISTORY_SIZE];
    uint8_t  history_index;
    uint16_t adc_min;
    uint16_t adc_max;
} Flex_Finger_t;

/* ── 全局双手数据 ── */
extern Flex_Finger_t Hand_Right[FINGER_NUM];
extern Flex_Finger_t Hand_Left[FINGER_NUM];

/* ── ★ Ping-Pong DMA 缓冲 (扩容为 5×2=10) ── */
extern volatile uint16_t adc1_buf[ADC_PINGPONG_SIZE];  /* ADC1: 右手 */
extern volatile uint16_t adc2_buf[ADC_PINGPONG_SIZE];  /* ADC2: 左手 */

/**
 * @brief Ping-Pong 就绪标志
 * @note  bit0 = ADC1 有新的稳定半组可读
 *        bit1 = ADC2 有新的稳定半组可读
 *        ISR 写入, 主循环读取后清零
 */
extern volatile uint8_t adc_pingpong_ready;

/**
 * @brief 当前稳定的半组索引
 * @note  0 = 前半组 (buf[0..4]) 稳定 (DMA 正写后半组)
 *        1 = 后半组 (buf[5..9]) 稳定 (DMA 正写前半组)
 *        高 4 bit = ADC2  (adc2_stable_half << 4)
 *        低 4 bit = ADC1  (adc1_stable_half)
 */
#define ADC_PP_HALF_ADC1_MASK  0x0FU
#define ADC_PP_HALF_ADC2_SHIFT 4U
#define ADC_PP_GET_ADC1(s)  ((s) & ADC_PP_HALF_ADC1_MASK)
#define ADC_PP_GET_ADC2(s)  (((s) >> ADC_PP_HALF_ADC2_SHIFT) & ADC_PP_HALF_ADC1_MASK)

extern volatile uint8_t adc_pingpong_half;  /* 高4bit=ADC2半组, 低4bit=ADC1半组 */

/* ── API ── */

void Flex_Init(void);

/**
 * @brief ★ Ping-Pong 快照更新 (替换旧 Flex_Update)
 * @note  读取 adc_pingpong_half → 选择稳定半组 → 拒绝 DMA 正在写的半组
 *         无 DMA 暂停! 无数据撕裂! 零 I2C 总线干扰!
 */
void Flex_Update(void);

uint8_t Flex_GetPercent(uint8_t hand, uint8_t finger);
uint8_t Flex_CheckSpasm(uint8_t hand, uint8_t finger);
uint32_t Flex_CalcSquaredDistance(uint8_t hand, uint8_t *standard_pose);
uint8_t Flex_HasValidSnapshot(uint8_t hand);
uint8_t Flex_GetSnapshotSeenMask(void);

#endif /* __FLEX_SENSOR_H */
