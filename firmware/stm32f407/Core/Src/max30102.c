/**
  ******************************************************************************
  * @file           : max30102.c
  * @brief          : MAX30102 心率血氧传感器驱动 — 硬件 I2C v3.0
  ******************************************************************************
  */
#include "max30102.h"
#include "bringup_diag.h"
#include "i2c.h"
#include "string.h"
#include "stm32f4xx_hal.h"

extern volatile uint32_t sys_tick_ms;

/* Start at moderate drive; the previous values saturated the black module. */
#define MAX30102_LED1_CURRENT   0x1CU  /* red LED, about 5.6 mA */
#define MAX30102_LED2_CURRENT   0x18U  /* IR LED,  about 4.8 mA */
#define MAX30102_HR_MIN         40U
#define MAX30102_HR_MAX         200U
#define MAX30102_BURST_MAX      32U
#define PPG_MIN_ANALYSIS_SAMPLES 200U
#define PPG_HR_WINDOW_SAMPLES   200U  /* 2 s at the configured 100 Hz */
#define PPG_HR_SMOOTH_RADIUS    2U    /* five-sample moving average */
#define PPG_HR_BASELINE_SAMPLES 25U   /* 0.25 s local DC estimate */
#define PPG_HR_MIN_AC_PP        50L
#define PPG_MIN_PEAK_INTERVALS  2U
#define PPG_MIN_INTERVAL_MS     300U
#define PPG_MAX_INTERVAL_MS     1500U
#define PPG_RESULT_UPDATE_MS    500U
#define PPG_RESULT_HOLD_MS      3000U
#define PPG_MIN_STABLE_RESULTS  3U

/* 8-bit I2C address for HAL (HAL expects address << 1 format) */
#define MAX30102_ADDR_HAL       MAX30102_I2C_ADDR  /* 0xAE */

/* ── Capture HAL error code for diagnostics ── */
static void max_capture_err(void)
{
    BringupDiag_SetMAX30102HalErr((uint8_t)(hi2c3.ErrorCode & 0xFFU));
    /* Clear error flags so next op can succeed */
    hi2c3.ErrorCode = HAL_I2C_ERROR_NONE;
}

/* ── Helper: HAL I2C read/write shorthands ── */
static HAL_StatusTypeDef max_read(uint8_t reg, uint8_t *val)
{
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c3, MAX30102_ADDR_HAL, reg,
                                            I2C_MEMADD_SIZE_8BIT, val, 1, 100);
    if (st != HAL_OK) max_capture_err();
    return st;
}
static HAL_StatusTypeDef max_write(uint8_t reg, uint8_t val)
{
    HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&hi2c3, MAX30102_ADDR_HAL, reg,
                                             I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    if (st != HAL_OK) max_capture_err();
    return st;
}
static HAL_StatusTypeDef max_read_buf(uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c3, MAX30102_ADDR_HAL, reg,
                                            I2C_MEMADD_SIZE_8BIT, buf, len, 100);
    if (st != HAL_OK) max_capture_err();
    return st;
}

static uint8_t max_fifo_reset_regs(void)
{
    uint8_t errors = 0U;

    if (max_write(MAX30102_FIFO_WR_PTR, 0x00) != HAL_OK) errors++;
    if (max_write(MAX30102_OVF_COUNTER, 0x00) != HAL_OK) errors++;
    if (max_write(MAX30102_FIFO_RD_PTR, 0x00) != HAL_OK) errors++;
    return errors;
}

/* ── 全局状态 ── */
MAX30102_Sample_t max30102_buf[MAX30102_BUF_LEN];
uint16_t          max30102_buf_idx = 0;
uint8_t           max30102_ready  = 0;
static uint16_t          max30102_sample_count = 0;
static uint8_t  hr_output    = 0;
static uint8_t  spo2_output  = 0;
static MAX30102_Quality_t ppg_quality;
static uint8_t  hr_history[5];
static uint8_t  hr_history_count = 0U;
static uint8_t  hr_history_index = 0U;
static float    spo2_ratio_filtered = 0.0f;
static uint8_t  spo2_ratio_valid = 0U;
static uint32_t ppg_last_good_ms = 0U;
static uint32_t ppg_last_result_update_ms = 0U;
static uint32_t ppg_hr_smooth[PPG_HR_WINDOW_SAMPLES];
static int32_t  ppg_hr_ac[PPG_HR_WINDOW_SAMPLES];

/* FIFO service diagnostics, retained across overflow recovery. */
static uint16_t fifo_calls      = 0;
static uint16_t fifo_resets     = 0;
static uint16_t fifo_reg_errors = 0;
static uint16_t fifo_data_errors = 0;
static uint16_t fifo_reset_write_errors = 0;
static uint16_t fifo_reset_verify_errors = 0;
static uint8_t  fifo_last_unread = 0;
static uint8_t  fifo_post_reset_wr = 0xFFU;
static uint8_t  fifo_post_reset_rd = 0xFFU;
static uint8_t  fifo_post_reset_ovf = 0xFFU;

static void ppg_process(void);

/* Return the middle value of the recent heart-rate candidates.  A median is
 * deliberately used here: one bad threshold crossing cannot pull the shown
 * value far away from the other four measurements. */
static uint8_t ppg_hr_history_median(void)
{
    uint8_t sorted[5];
    uint8_t count = hr_history_count;

    if (count == 0U) return 0U;
    memcpy(sorted, hr_history, count);
    for (uint8_t i = 0U; i + 1U < count; i++) {
        for (uint8_t j = i + 1U; j < count; j++) {
            if (sorted[i] > sorted[j]) {
                uint8_t temp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = temp;
            }
        }
    }
    return sorted[count / 2U];
}

static uint8_t ppg_spo2_from_ratio(float ratio)
{
    float estimate = 110.0f - (25.0f * ratio);
    if (estimate > 100.0f) estimate = 100.0f;
    if (estimate < 70.0f) estimate = 70.0f;
    return (uint8_t)(estimate + 0.5f);
}

static void ppg_expire_stale_result(void)
{
    if (ppg_last_good_ms == 0U ||
        (uint32_t)(sys_tick_ms - ppg_last_good_ms) > PPG_RESULT_HOLD_MS) {
        hr_output = 0U;
        spo2_output = 0U;
    }
}

static void ppg_accept_result(uint8_t hr_candidate, float ratio_candidate)
{
    ppg_last_good_ms = sys_tick_ms;

    /* The sliding window advances much more often than a human-readable
     * display needs.  Rate-limit result replacement without discarding the
     * proof that a good waveform is still present. */
    if (ppg_last_result_update_ms != 0U &&
        (uint32_t)(sys_tick_ms - ppg_last_result_update_ms) < PPG_RESULT_UPDATE_MS) {
        return;
    }

    hr_history[hr_history_index] = hr_candidate;
    hr_history_index = (hr_history_index + 1U) % 5U;
    if (hr_history_count < 5U) hr_history_count++;

    /* Finger placement creates a large optical transient.  Do not publish a
     * medical-looking value until three independent, good windows agree. */
    if (hr_history_count < PPG_MIN_STABLE_RESULTS) {
        hr_output = 0U;
        spo2_output = 0U;
        spo2_ratio_valid = 0U;
        ppg_last_result_update_ms = sys_tick_ms;
        return;
    }

    hr_output = ppg_hr_history_median();

    if (spo2_ratio_valid == 0U) {
        spo2_ratio_filtered = ratio_candidate;
        spo2_ratio_valid = 1U;
    } else {
        /* Keep 75% of the earlier estimate and blend in 25% of this window. */
        spo2_ratio_filtered = (0.75f * spo2_ratio_filtered) + (0.25f * ratio_candidate);
    }
    spo2_output = ppg_spo2_from_ratio(spo2_ratio_filtered);
    ppg_last_result_update_ms = sys_tick_ms;
}

static void max_record_post_reset(void)
{
    if (max_read(MAX30102_FIFO_WR_PTR, &fifo_post_reset_wr) != HAL_OK) {
        fifo_reset_verify_errors++;
    }
    if (max_read(MAX30102_FIFO_RD_PTR, &fifo_post_reset_rd) != HAL_OK) {
        fifo_reset_verify_errors++;
    }
    if (max_read(MAX30102_OVF_COUNTER, &fifo_post_reset_ovf) != HAL_OK) {
        fifo_reset_verify_errors++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  传感器初始化
 * ═══════════════════════════════════════════════════════════════════════════ */

uint8_t MAX30102_Init(void)
{
    uint8_t reg_val;
    BringupDiag_SetMAX30102PartId(0x00U);

    /* Step 0: POR delay — MAX30102 needs ~100ms after VCC stable */
    HAL_Delay(100);

    /* Step 1: 软复位 */
    if (max_write(MAX30102_MODE_CONFIG, MAX30102_RESET) != HAL_OK) {
        return 1;
    }
    HAL_Delay(10);

    /* Step 2: 验证 PART_ID */
    if (max_read(MAX30102_PART_ID, &reg_val) != HAL_OK) {
        return 2;
    }
    BringupDiag_SetMAX30102PartId(reg_val);
    if (reg_val != MAX30102_PART_ID_VAL) return 3;

    /* Step 3: 禁用中断 */
    max_write(MAX30102_INT_ENABLE1, 0x00);
    max_write(MAX30102_INT_ENABLE2, 0x00);

    /* Step 4: FIFO 配置 */
    max_write(MAX30102_FIFO_CONFIG, MAX30102_FIFO_ROLLOVER);

    /* Step 5: SpO2 配置 → 100Hz + 18-bit */
    max_write(MAX30102_SPO2_CONFIG, MAX30102_SPO2_CFG_100HZ);

    /* Step 6: LED 电流 */
    max_write(MAX30102_LED1_PA, MAX30102_LED1_CURRENT);
    max_write(MAX30102_LED2_PA, MAX30102_LED2_CURRENT);

    /* Step 7: 多 LED 模式 */
    max_write(MAX30102_MULTI_LED_CTRL1, 0x21);
    max_write(MAX30102_MULTI_LED_CTRL2, 0x00);

    /* Step 8: 进入 SpO2 模式 */
    max_write(MAX30102_MODE_CONFIG, MAX30102_MODE_SPO2);
    HAL_Delay(10);

    /* Step 9: 清除 FIFO 残留 */
    max_fifo_reset_regs();

    /* Step 10: 初始化内部状态 */
    MAX30102_ResetHistory();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  FIFO 突发清仓读取
 * ═══════════════════════════════════════════════════════════════════════════ */

void MAX30102_ReadFIFO(void)
{
    uint8_t wr_ptr, rd_ptr;
    uint8_t raw[6];
    HAL_StatusTypeDef st;

    fifo_calls++;

    /* ── 1. 读取 FIFO 读写指针 ── */
    st = max_read(MAX30102_FIFO_WR_PTR, &wr_ptr);
    if (st != HAL_OK) {
        fifo_reg_errors++;
        return;
    }
    st = max_read(MAX30102_FIFO_RD_PTR, &rd_ptr);
    if (st != HAL_OK) {
        fifo_reg_errors++;
        return;
    }

    /* ── 2. 计算 FIFO 中未读样本数 ── */
    int8_t unread = (int8_t)(wr_ptr - rd_ptr);
    if (unread < 0) unread += (int8_t)MAX30102_FIFO_DEPTH;
    fifo_last_unread = (uint8_t)unread;

    if (unread <= 0) {
        /*
         * This module reports FIFO_OVF_COUNTER=31 permanently even after a
         * successful pointer reset. Use only the FIFO pointers for flow
         * control; the counter remains in the diagnostic report only.
         */
        return;
    }

    /* ── 3. 溢出检测 ── */
    /* ── 4. 防独占保护 ── */
    if (unread > (int8_t)MAX30102_BURST_MAX) {
        fifo_resets++;
        if (max_fifo_reset_regs() != 0U) fifo_reset_write_errors++;
        max_record_post_reset();
        MAX30102_ResetHistory();
        return;
    }

    /* ── 5. 突发清仓循环 ── */
    while (unread > 0) {
        st = max_read_buf(MAX30102_FIFO_DATA, raw, 6);
        if (st != HAL_OK) {
            fifo_data_errors++;
            return;
        }

        uint32_t red_val = (((uint32_t)raw[0] << 16)
                         | ((uint32_t)raw[1] << 8)
                         |  (uint32_t)raw[2]) & MAX30102_FIFO_SAMPLE_MASK;
        uint32_t ir_val  = (((uint32_t)raw[3] << 16)
                         | ((uint32_t)raw[4] << 8)
                         |  (uint32_t)raw[5]) & MAX30102_FIFO_SAMPLE_MASK;

        uint8_t sample_valid = 1U;
        if (ir_val < (uint32_t)MAX30102_IR_MIN_VALID ||
            ir_val > (uint32_t)MAX30102_IR_MAX_VALID) {
            sample_valid = 0U;
            MAX30102_ResetHistory();
        }

        max30102_buf[max30102_buf_idx].ir      = ir_val;
        max30102_buf[max30102_buf_idx].red     = red_val;
        max30102_buf[max30102_buf_idx].valid   = sample_valid;
        max30102_buf[max30102_buf_idx].tick_ms = sys_tick_ms;
        max30102_buf_idx = (max30102_buf_idx + 1U) % MAX30102_BUF_LEN;
        if (max30102_sample_count < MAX30102_BUF_LEN) {
            max30102_sample_count++;
        }
        max30102_ready = 1U;

        unread--;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PPG 信号处理
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ppg_process(void)
{
    uint16_t total = max30102_sample_count;
    uint16_t oldest;
    uint16_t valid_count = 0U;
    uint32_t ir_sum = 0U, red_sum = 0U;
    uint32_t ir_min = 0xFFFFFFFFU, ir_max = 0U;
    uint32_t red_min = 0xFFFFFFFFU, red_max = 0U;
    uint32_t intervals[5] = {0U};
    uint8_t interval_count = 0U;
    uint8_t above = 0U;
    uint32_t last_cross_ms = 0U;

    memset(&ppg_quality, 0, sizeof(ppg_quality));
    ppg_quality.sample_count = total;

    if (total < PPG_MIN_ANALYSIS_SAMPLES) {
        ppg_expire_stale_result();
        max30102_ready = 0U;
        return;
    }

    oldest = (total < MAX30102_BUF_LEN) ? 0U : max30102_buf_idx;

    for (uint16_t i = 0U; i < total; i++) {
        uint16_t idx = oldest + i;
        if (idx >= MAX30102_BUF_LEN) idx -= MAX30102_BUF_LEN;
        if (!max30102_buf[idx].valid) continue;

        valid_count++;
        ir_sum += max30102_buf[idx].ir;
        red_sum += max30102_buf[idx].red;
        if (max30102_buf[idx].ir < ir_min) ir_min = max30102_buf[idx].ir;
        if (max30102_buf[idx].ir > ir_max) ir_max = max30102_buf[idx].ir;
        if (max30102_buf[idx].red < red_min) red_min = max30102_buf[idx].red;
        if (max30102_buf[idx].red > red_max) red_max = max30102_buf[idx].red;
    }

    if (valid_count < PPG_MIN_ANALYSIS_SAMPLES || ir_max <= ir_min || red_max <= red_min) {
        ppg_quality.valid_samples = valid_count;
        ppg_expire_stale_result();
        max30102_ready = 0U;
        return;
    }

    {
        uint32_t ir_dc = ir_sum / valid_count;
        uint32_t red_dc = red_sum / valid_count;
        uint32_t ir_pp = ir_max - ir_min;
        uint32_t red_pp = red_max - red_min;
        uint16_t hr_total = (total > PPG_HR_WINDOW_SAMPLES)
                          ? PPG_HR_WINDOW_SAMPLES : total;
        uint16_t hr_oldest = (max30102_buf_idx >= hr_total)
                           ? (max30102_buf_idx - hr_total)
                           : (MAX30102_BUF_LEN + max30102_buf_idx - hr_total);
        uint16_t hr_valid_count = 0U;
        int32_t ac_min = 0x7FFFFFFFL, ac_max = (-0x7FFFFFFFL - 1L);
        int32_t high_threshold = 0L, low_threshold = 0L;
        uint32_t baseline_sum = 0U;
        uint8_t hr_candidate = 0U;
        float ratio_candidate = 0.0f;
        uint8_t quality_ok = 0U;

        ppg_quality.valid_samples = valid_count;
        ppg_quality.ir_dc = ir_dc;
        ppg_quality.red_dc = red_dc;
        ppg_quality.ir_pp = ir_pp;
        ppg_quality.red_pp = red_pp;

        /* Use only the newest two seconds to locate beats. */
        for (uint16_t i = 0U; i < hr_total; i++) {
            uint16_t idx = hr_oldest + i;
            if (idx >= MAX30102_BUF_LEN) idx -= MAX30102_BUF_LEN;
            if (!max30102_buf[idx].valid) continue;
            hr_valid_count++;
        }

        if (hr_valid_count >= PPG_MIN_ANALYSIS_SAMPLES) {
            /* First remove high-frequency noise with a five-point average. */
            for (uint16_t i = 0U; i < hr_total; i++) {
                uint32_t sum = 0U;
                uint8_t count = 0U;
                uint16_t begin = (i > PPG_HR_SMOOTH_RADIUS)
                               ? (i - PPG_HR_SMOOTH_RADIUS) : 0U;
                uint16_t end = i + PPG_HR_SMOOTH_RADIUS;
                if (end >= hr_total) end = hr_total - 1U;
                for (uint16_t j = begin; j <= end; j++) {
                    uint16_t idx = hr_oldest + j;
                    if (idx >= MAX30102_BUF_LEN) idx -= MAX30102_BUF_LEN;
                    sum += max30102_buf[idx].ir;
                    count++;
                }
                ppg_hr_smooth[i] = sum / count;
            }

            /* Subtract a local 250-ms DC baseline.  Finger pressure changes
             * slowly, while the pulse remains as a small AC component. */
            for (uint16_t i = 0U; i < hr_total; i++) {
                if (i > 0U) baseline_sum += ppg_hr_smooth[i - 1U];
                if (i > PPG_HR_BASELINE_SAMPLES) {
                    baseline_sum -= ppg_hr_smooth[i - PPG_HR_BASELINE_SAMPLES - 1U];
                }
                if (i >= PPG_HR_BASELINE_SAMPLES) {
                    ppg_hr_ac[i] = (int32_t)ppg_hr_smooth[i]
                                 - (int32_t)(baseline_sum / PPG_HR_BASELINE_SAMPLES);
                    if (ppg_hr_ac[i] < ac_min) ac_min = ppg_hr_ac[i];
                    if (ppg_hr_ac[i] > ac_max) ac_max = ppg_hr_ac[i];
                } else {
                    ppg_hr_ac[i] = 0L;
                }
            }
        }

        if (hr_valid_count >= PPG_MIN_ANALYSIS_SAMPLES &&
            (ac_max - ac_min) >= PPG_HR_MIN_AC_PP) {
            ppg_quality.hr_ac_pp = (uint32_t)(ac_max - ac_min);
            high_threshold = ac_min + ((ac_max - ac_min) * 6L / 10L);
            low_threshold = ac_min + ((ac_max - ac_min) * 4L / 10L);

            for (uint16_t i = 0U; i < hr_total; i++) {
                uint16_t idx = hr_oldest + i;
                uint32_t interval;
                if (idx >= MAX30102_BUF_LEN) idx -= MAX30102_BUF_LEN;
                if (!max30102_buf[idx].valid || i < PPG_HR_BASELINE_SAMPLES) {
                    above = 0U;
                    continue;
                }

                if (!above && ppg_hr_ac[i] >= high_threshold) {
                    if (last_cross_ms != 0U) {
                        interval = max30102_buf[idx].tick_ms - last_cross_ms;
                        if (interval >= PPG_MIN_INTERVAL_MS && interval <= PPG_MAX_INTERVAL_MS) {
                            if (interval_count < 5U) {
                                intervals[interval_count++] = interval;
                            } else {
                                for (uint8_t j = 0U; j < 4U; j++) intervals[j] = intervals[j + 1U];
                                intervals[4] = interval;
                            }
                        }
                    }
                    last_cross_ms = max30102_buf[idx].tick_ms;
                    above = 1U;
                } else if (above && ppg_hr_ac[i] <= low_threshold) {
                    above = 0U;
                }
            }
        }

        if (interval_count >= PPG_MIN_PEAK_INTERVALS) {
            for (uint8_t a = 0U; a + 1U < interval_count; a++) {
                for (uint8_t b = a + 1U; b < interval_count; b++) {
                    if (intervals[a] > intervals[b]) {
                        uint32_t t = intervals[a];
                        intervals[a] = intervals[b];
                        intervals[b] = t;
                    }
                }
            }
            {
                uint32_t median_interval = intervals[interval_count / 2U];
                ppg_quality.median_interval_ms = (uint16_t)median_interval;
                hr_candidate = (uint8_t)(60000UL / median_interval);
                if (hr_candidate < MAX30102_HR_MIN) hr_candidate = MAX30102_HR_MIN;
                if (hr_candidate > MAX30102_HR_MAX) hr_candidate = MAX30102_HR_MAX;
            }
        }
        ppg_quality.peak_intervals = interval_count;

        if (interval_count >= PPG_MIN_PEAK_INTERVALS &&
            ir_pp >= (ir_dc / 1000U) && red_pp >= (red_dc / 1000U) &&
            ir_dc > 0U && red_dc > 0U) {
            ratio_candidate = ((float)red_pp * (float)ir_dc) /
                              ((float)ir_pp * (float)red_dc);
            if (ratio_candidate >= 0.20f && ratio_candidate <= 2.00f) {
                ppg_quality.ratio_x1000 = (uint16_t)(ratio_candidate * 1000.0f + 0.5f);
                quality_ok = 1U;
            }
        }

        if (quality_ok != 0U && hr_candidate != 0U) {
            ppg_accept_result(hr_candidate, ratio_candidate);
        } else {
            ppg_expire_stale_result();
        }
    }

    max30102_ready = 0U;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════════════════════════════════════ */

uint8_t MAX30102_GetHR(void)   { return hr_output; }
uint8_t MAX30102_GetSpO2(void) { return spo2_output; }

void MAX30102_ResetHistory(void)
{
    memset(max30102_buf, 0, sizeof(max30102_buf));
    max30102_buf_idx = 0; max30102_ready = 0;
    max30102_sample_count = 0U;
    hr_output = 0;
    spo2_output = 0;
    memset(hr_history, 0, sizeof(hr_history));
    hr_history_count = 0U;
    hr_history_index = 0U;
    spo2_ratio_filtered = 0.0f;
    spo2_ratio_valid = 0U;
    ppg_last_good_ms = 0U;
    ppg_last_result_update_ms = 0U;
    memset(&ppg_quality, 0, sizeof(ppg_quality));
}

uint8_t MAX30102_IsOnline(void)
{
    uint8_t valid_cnt = 0;
    for (uint16_t i = 0U; i < 3U && i < MAX30102_BUF_LEN; i++) {
        uint16_t idx = (max30102_buf_idx >= i + 1U)
                      ? (max30102_buf_idx - i - 1U)
                      : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);
        if (max30102_buf[idx].valid) valid_cnt++;
    }
    return (valid_cnt >= 2U) ? 1U : 0U;
}

uint8_t MAX30102_GetDebugRegs(MAX30102_DebugRegs_t *out)
{
    if (out == NULL) {
        return 1U;
    }

    if (max_read(MAX30102_FIFO_WR_PTR, &out->wr_ptr) != HAL_OK) {
        return 2U;
    }
    if (max_read(MAX30102_FIFO_RD_PTR, &out->rd_ptr) != HAL_OK) {
        return 3U;
    }
    if (max_read(MAX30102_OVF_COUNTER, &out->ovf_cnt) != HAL_OK) {
        return 4U;
    }
    if (max_read(MAX30102_MODE_CONFIG, &out->mode_cfg) != HAL_OK) {
        return 5U;
    }
    if (max_read(MAX30102_SPO2_CONFIG, &out->spo2_cfg) != HAL_OK) {
        return 6U;
    }

    out->fifo_calls = fifo_calls;
    out->fifo_resets = fifo_resets;
    out->fifo_reg_errors = fifo_reg_errors;
    out->fifo_data_errors = fifo_data_errors;
    out->fifo_reset_write_errors = fifo_reset_write_errors;
    out->fifo_reset_verify_errors = fifo_reset_verify_errors;
    out->last_unread = fifo_last_unread;
    out->post_reset_wr_ptr = fifo_post_reset_wr;
    out->post_reset_rd_ptr = fifo_post_reset_rd;
    out->post_reset_ovf_cnt = fifo_post_reset_ovf;

    return 0U;
}

uint8_t MAX30102_GetQuality(MAX30102_Quality_t *out)
{
    if (out == NULL) {
        return 1U;
    }
    *out = ppg_quality;
    return 0U;
}

void MAX30102_ProcessTick(void)
{
    if (max30102_ready) ppg_process();
}
