/**
  ******************************************************************************
  * @file           : flex_sensor.c
  * @brief          : 柔性传感器驱动 - Ping-Pong 双缓冲快照 v2.4
  * @author         : 架构维护版 v3.1
  * @date           : 2026-06-29
  ******************************************************************************
  */

#include "flex_sensor.h"
#include "stdlib.h"

/* 双手五指状态 */
Flex_Finger_t Hand_Right[FINGER_NUM];
Flex_Finger_t Hand_Left[FINGER_NUM];

/* DMA Ping-Pong 状态 */
volatile uint8_t adc_pingpong_ready = 0;  /* bit0=ADC1, bit1=ADC2 */
volatile uint8_t adc_pingpong_half  = 0;  /* 高 4bit=ADC2, 低 4bit=ADC1 */
static uint8_t adc_snapshot_seen = 0;     /* bit0=ADC1 ever seen, bit1=ADC2 ever seen */

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
            for (uint8_t j = 0; j < HISTORY_SIZE; j++) {
                hand[i].history_buffer[j] = 0;
            }
        }
    }
    adc_pingpong_ready = 0;
    adc_pingpong_half  = 0;
    adc_snapshot_seen  = 0;
}

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

void Flex_Update(void)
{
    uint8_t ready = adc_pingpong_ready;
    uint8_t half  = adc_pingpong_half;
    adc_pingpong_ready = 0;

    if (ready & 0x01U) adc_snapshot_seen |= 0x01U;
    if (ready & 0x02U) adc_snapshot_seen |= 0x02U;

    uint8_t half_adc1 = ADC_PP_GET_ADC1(half);
    uint8_t half_adc2 = ADC_PP_GET_ADC2(half);

    for (uint8_t h = 0; h < 2; h++) {
        Flex_Finger_t *hand = h ? Hand_Left : Hand_Right;
        volatile uint16_t *buf_full = h ? adc2_buf : adc1_buf;
        uint8_t stable_half = h ? half_adc2 : half_adc1;
        uint8_t has_fresh_snapshot = h ? ((ready & 0x02U) != 0U)
                                        : ((ready & 0x01U) != 0U);

        if (!has_fresh_snapshot) {
            continue;
        }

        const volatile uint16_t *snap = &buf_full[stable_half * FINGER_NUM];

        for (uint8_t i = 0; i < FINGER_NUM; i++) {
            uint16_t raw_new = snap[i];

            if (hand[i].current_raw == 0) {
                hand[i].current_raw = raw_new;
            } else {
                hand[i].current_raw = (hand[i].current_raw * 3U + raw_new) / 4U;
            }
            uint16_t filtered = hand[i].current_raw;

            if (filtered < hand[i].adc_min) hand[i].adc_min = filtered;
            if (filtered > hand[i].adc_max) hand[i].adc_max = filtered;

            hand[i].pos_percent = compute_percent(filtered,
                hand[i].adc_min, hand[i].adc_max);

            hand[i].history_buffer[hand[i].history_index] = hand[i].pos_percent;
            hand[i].history_index++;
            if (hand[i].history_index >= HISTORY_SIZE) {
                hand[i].history_index = 0;
            }
        }
    }
}

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
        for (uint8_t j = 0; j < HISTORY_SIZE; j++) {
            f->history_buffer[j] = f->pos_percent;
        }
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

uint8_t Flex_HasValidSnapshot(uint8_t hand)
{
    if (hand > 1U) return 0U;
    return (adc_snapshot_seen & (1U << hand)) ? 1U : 0U;
}

uint8_t Flex_GetSnapshotSeenMask(void)
{
    return adc_snapshot_seen;
}
