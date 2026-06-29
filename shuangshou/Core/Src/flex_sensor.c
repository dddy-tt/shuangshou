#include "flex_sensor.h"
#include "stdlib.h"

/* ── 双手手指数据 ── */
Flex_Finger_t Hand_Right[FINGER_NUM];
Flex_Finger_t Hand_Left[FINGER_NUM];

/* ── 初始化：极值反向初始化 ── */
void Flex_Init(void)
{
    for (uint8_t h = 0; h < 2; h++) {
        Flex_Finger_t *hand = h ? Hand_Left : Hand_Right;
        for (uint8_t i = 0; i < FINGER_NUM; i++) {
            hand[i].adc_min = 4095;  /* 故意反向：min 设到顶 */
            hand[i].adc_max = 0;     /*          max 设到底 */
            hand[i].pos_percent = 0;
            hand[i].current_raw = 0;
            hand[i].history_index = 0;
            for (uint8_t j = 0; j < HISTORY_SIZE; j++) {
                hand[i].history_buffer[j] = 0;
            }
        }
    }
}

/* ── 动态百分比计算（线性插值 + 死区钳位） ── */
static uint8_t compute_percent(uint16_t raw, uint16_t min_cal, uint16_t max_cal)
{
    int32_t diff = (int32_t)max_cal - (int32_t)min_cal;
    /* 防死机：极值太近 */
    if (diff > -50 && diff < 50) return 0;

    float percent = 0.0f;
    if (max_cal > min_cal) {
        /* 正向拉伸：ADC 越大弯曲越多 */
        if ((int32_t)raw <= (int32_t)min_cal) return 0;
        if ((int32_t)raw >= (int32_t)max_cal) return 100;
        percent = (float)((int32_t)raw - (int32_t)min_cal) /
                  (float)((int32_t)max_cal - (int32_t)min_cal) * 100.0f;
    } else {
        /* 反向压缩：ADC 越小弯曲越多 */
        if ((int32_t)raw >= (int32_t)min_cal) return 0;
        if ((int32_t)raw <= (int32_t)max_cal) return 100;
        percent = (float)((int32_t)min_cal - (int32_t)raw) /
                  (float)((int32_t)min_cal - (int32_t)max_cal) * 100.0f;
    }
    return (uint8_t)percent;
}

/* ── 更新：ADC DMA 缓冲 → EMA 滤波 → 百分比 ── */
void Flex_Update(void)
{
    for (uint8_t h = 0; h < 2; h++) {
        Flex_Finger_t *hand = h ? Hand_Left : Hand_Right;
        volatile uint16_t *buf = h ? adc2_buf : adc1_buf;

        for (uint8_t i = 0; i < FINGER_NUM; i++) {
            uint16_t raw_new = buf[i];

            /* EMA 低通滤波：new = (old*3 + new)/4 */
            if (hand[i].current_raw == 0) {
                hand[i].current_raw = raw_new;
            } else {
                hand[i].current_raw =
                    (hand[i].current_raw * 3 + raw_new) / 4;
            }
            uint16_t filtered = hand[i].current_raw;

            /* 更新极值（开机自动学习） */
            if (filtered < hand[i].adc_min) hand[i].adc_min = filtered;
            if (filtered > hand[i].adc_max) hand[i].adc_max = filtered;

            /* 百分比映射 */
            hand[i].pos_percent = compute_percent(filtered,
                hand[i].adc_min, hand[i].adc_max);

            /* 维护滑动窗口 */
            hand[i].history_buffer[hand[i].history_index] =
                hand[i].pos_percent;
            hand[i].history_index++;
            if (hand[i].history_index >= HISTORY_SIZE) {
                hand[i].history_index = 0;
            }
        }
    }
}

/* ── 获取百分比 ── */
uint8_t Flex_GetPercent(uint8_t hand, uint8_t finger)
{
    if (finger >= FINGER_NUM) return 0;
    Flex_Finger_t *h = hand ? Hand_Left : Hand_Right;
    return h[finger].pos_percent;
}

/* ── 痉挛检测 ── */
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
        /* 冷却机制：清空历史 */
        for (uint8_t j = 0; j < HISTORY_SIZE; j++) {
            f->history_buffer[j] = f->pos_percent;
        }
        return 1;
    }
    return 0;
}

/* ── 模板距离 ── */
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
