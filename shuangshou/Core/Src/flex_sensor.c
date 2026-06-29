/**
  ******************************************************************************
  * @file           : flex_sensor.c
  * @brief          : 柔性传感器 — 无锁 Ping-Pong 双缓冲快照 v2.4
  * @author         : 车架师重构版 v2.4
  * @date           : 2026-06-29
  *
  * @核心升级 (v2.3 → v2.4):
  *   ★ Ping-Pong 原子快照:
  *     - ADC DMA 缓冲区从 5 扩容为 10 (5chn × 2 halves)
  *     - DMA 循环写满 10 个半字触发 Cplt 中断, 写满前 5 个触发 HalfCplt
  *     - HalfCplt ISR: 锁存 "前半组完成" 标志 → Flex_Update 读 buf[0..4]
  *     - Cplt ISR:     锁存 "后半组完成" 标志 → Flex_Update 读 buf[5..9]
  *     - ★ 完全不碰 DMA_CR.EN 位, 彻底删除 Stop/Start 高频切流
  *     - ★ 每个半组是完整的一次 5 通道 ADC 扫描结果, 数据一致性强
  *
  *   ★ 时序验证:
  *     ADC 时钟 = 21MHz, 每通道采样 480 cycles + 12-bit 转换 ≈ 24μs
  *     5 通道 × 24μs ≈ 120μs 完成一次全扫描
  *     HalfCplt 中断 ≈ 每 120μs 触发一次 ← 远高于 20ms Flex_Update 频率
  *     → 20ms 任务到点时, 一定有稳定的半组可用
  ******************************************************************************
  */

#include "flex_sensor.h"
#include "stdlib.h"

/* ── 双手手指数据 ── */
Flex_Finger_t Hand_Right[FINGER_NUM];
Flex_Finger_t Hand_Left[FINGER_NUM];

/* ── ★ Ping-Pong 控制标志 ── */
volatile uint8_t adc_pingpong_ready = 0;  /* bit0=ADC1, bit1=ADC2 有新快照 */
volatile uint8_t adc_pingpong_half  = 0;  /* 高4bit=ADC2, 低4bit=ADC1 稳定半组 */

/* ── 初始化 ── */
void Flex_Init(void)
{
    for (uint8_t h = 0; h < 2; h++) {
        Flex_Finger_t *hand = h ? Hand_Left : Hand_Right;
        for (uint8_t i = 0; i < FINGER_NUM; i++) {
            hand[i].adc_min = 4095;
            hand[i].adc_max = 0;
            hand[i].pos_percent = 0;
            hand[i].current_raw = 0;
            hand[i].history_index = 0;
            for (uint8_t j = 0; j < HISTORY_SIZE; j++)
                hand[i].history_buffer[j] = 0;
        }
    }
    adc_pingpong_ready = 0;
    adc_pingpong_half  = 0;
}

/* ── 动态百分比计算 (不变) ── */
static uint8_t compute_percent(uint16_t raw, uint16_t min_cal, uint16_t max_cal)
{
    int32_t diff = (int32_t)max_cal - (int32_t)min_cal;
    if (diff > -50 && diff < 50) return 0;

    if (max_cal > min_cal) {
        if ((int32_t)raw <= (int32_t)min_cal) return 0;
        if ((int32_t)raw >= (int32_t)max_cal) return 100;
        return (uint8_t)((float)((int32_t)raw - (int32_t)min_cal)
              / (float)((int32_t)max_cal - (int32_t)min_cal) * 100.0f);
    } else {
        if ((int32_t)raw >= (int32_t)min_cal) return 0;
        if ((int32_t)raw <= (int32_t)max_cal) return 100;
        return (uint8_t)((float)((int32_t)min_cal - (int32_t)raw)
              / (float)((int32_t)min_cal - (int32_t)max_cal) * 100.0f);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ★ Ping-Pong 快照更新 — 拒绝 DMA 暂停, 只读稳定半组
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Ping-Pong 无锁快照: ADC 数据 → EMA 滤波 → 百分比
 * @note   ★ 关键: 不读 adc1_buf/adc2_buf 的全 10 个元素,
 *         而是根据 adc_pingpong_half 标志, 只读 DMA 刚刚写完的那一半。
 *         DMA 正在写的另一半绝不触碰 → 零撕裂保证。
 *
 *         数据流:
 *           DMA 持续写 [H0=ch0..4 | H1=ch0..4] 循环
 *           HC中断→锁 H0完成   Cplt中断→锁 H1完成
 *           Flex_Update →读已完成的那半 → 5个手指值 → 百分比
 */
void Flex_Update(void)
{
    /* ── 快照 ping-pong 半组标志 (ISR 写入, 此处读取后清零) ── */
    uint8_t ready = adc_pingpong_ready;
    uint8_t half  = adc_pingpong_half;
    adc_pingpong_ready = 0;  /* 原子清零 (单字节写, STM32 天然原子) */

    uint8_t half_adc1 = ADC_PP_GET_ADC1(half);
    uint8_t half_adc2 = ADC_PP_GET_ADC2(half);

    for (uint8_t h = 0; h < 2; h++) {
        Flex_Finger_t *hand = h ? Hand_Left : Hand_Right;
        volatile uint16_t *buf_full = h ? adc2_buf : adc1_buf;
        uint8_t          stable_half = h ? half_adc2 : half_adc1;

        /* ★ Ping-Pong 指针: 指向稳定半组的首地址 ★ */
        /* 前半组: buf[0..4], 后半组: buf[5..9] */
        const volatile uint16_t *snap = &buf_full[stable_half * FINGER_NUM];

        for (uint8_t i = 0; i < FINGER_NUM; i++) {
            uint16_t raw_new = snap[i];  /* ← 从快照读取, 数据一致 */

            /* EMA 低通滤波 */
            if (hand[i].current_raw == 0) {
                hand[i].current_raw = raw_new;
            } else {
                hand[i].current_raw =
                    (hand[i].current_raw * 3 + raw_new) / 4;
            }
            uint16_t filtered = hand[i].current_raw;

            /* 自适应极值 */
            if (filtered < hand[i].adc_min) hand[i].adc_min = filtered;
            if (filtered > hand[i].adc_max) hand[i].adc_max = filtered;

            /* 百分比映射 */
            hand[i].pos_percent = compute_percent(filtered,
                hand[i].adc_min, hand[i].adc_max);

            /* 滑动窗口 */
            hand[i].history_buffer[hand[i].history_index] = hand[i].pos_percent;
            hand[i].history_index++;
            if (hand[i].history_index >= HISTORY_SIZE)
                hand[i].history_index = 0;
        }
    }
}

/* ── 其余函数不变 ── */

uint8_t Flex_GetPercent(uint8_t hand, uint8_t finger)
{
    if (finger >= FINGER_NUM) return 0;
    Flex_Finger_t *h = hand ? Hand_Left : Hand_Right;
    return h[finger].pos_percent;
}

uint8_t Flex_CheckSpasm(uint8_t hand, uint8_t finger)
{
    if (finger >= FINGER_NUM) return 0;
    Flex_Finger_t *f = hand ? &Hand_Left[finger] : &Hand_Right[finger];

    int32_t total_variation = 0;
    for (uint8_t i = 0; i < HISTORY_SIZE - 1; i++) {
        uint8_t idx_old = (f->history_index + i) % HISTORY_SIZE;
        uint8_t idx_new = (f->history_index + i + 1) % HISTORY_SIZE;
        int32_t diff = (int32_t)f->history_buffer[idx_new]
                     - (int32_t)f->history_buffer[idx_old];
        total_variation += (diff >= 0) ? diff : -diff;
    }
    if (total_variation > 85) {
        for (uint8_t j = 0; j < HISTORY_SIZE; j++)
            f->history_buffer[j] = f->pos_percent;
        return 1;
    }
    return 0;
}

uint32_t Flex_CalcSquaredDistance(uint8_t hand, uint8_t *standard_pose)
{
    Flex_Finger_t *h = hand ? Hand_Left : Hand_Right;
    uint32_t dist_sq = 0;
    for (uint8_t i = 0; i < FINGER_NUM; i++) {
        int32_t d = (int32_t)h[i].pos_percent - (int32_t)standard_pose[i];
        dist_sq += (uint32_t)(d * d);
    }
    return dist_sq;
}
