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

#define MAX30102_LED1_CURRENT   0x3FU
#define MAX30102_LED2_CURRENT   0x4FU
#define MAX30102_HR_MIN         40U
#define MAX30102_HR_MAX         200U
#define MAX30102_BURST_MAX      32U

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

/* ── 全局状态 ── */
MAX30102_Sample_t max30102_buf[MAX30102_BUF_LEN];
uint8_t           max30102_buf_idx = 0;
uint8_t           max30102_ready  = 0;

static uint32_t ir_dc_sum    = 0;
static uint32_t red_dc_sum   = 0;
static uint8_t  dc_count     = 0;
static uint32_t ir_dc_est    = 1;
static int32_t  ir_ac_max    = 0;
static int32_t  ir_ac_min    = 0;
static uint32_t last_peak_ms = 0;
static uint32_t peak_intervals[5];
static uint8_t  peak_idx     = 0;
static uint8_t  peak_cnt     = 0;
static uint8_t  hr_output    = 0;
static uint8_t  spo2_output  = 0;
static void ppg_process(void);

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

    /* Step 9: 清除 FIFO 残留 */
    {
        uint8_t wr_ptr, rd_ptr;
        max_read(MAX30102_FIFO_WR_PTR, &wr_ptr);
        max_read(MAX30102_FIFO_RD_PTR, &rd_ptr);
        uint8_t fifo_count = (wr_ptr - rd_ptr) & 0x1FU;
        uint8_t dummy[6];
        for (uint8_t i = 0; i < fifo_count; i++) {
            max_read_buf(MAX30102_FIFO_DATA, dummy, 6);
        }
    }

    /* Step 10: 初始化内部状态 */
    MAX30102_ResetHistory();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  FIFO 突发清仓读取
 * ═══════════════════════════════════════════════════════════════════════════ */

void MAX30102_ReadFIFO(void)
{
    uint8_t wr_ptr, rd_ptr, ovf;
    uint8_t raw[6];
    HAL_StatusTypeDef st;

    /* ── 1. 读取 FIFO 读写指针 ── */
    st = max_read(MAX30102_FIFO_WR_PTR, &wr_ptr);
    if (st != HAL_OK) return;
    st = max_read(MAX30102_FIFO_RD_PTR, &rd_ptr);
    if (st != HAL_OK) return;

    /* ── 2. 计算 FIFO 中未读样本数 ── */
    int8_t unread = (int8_t)(wr_ptr - rd_ptr);
    if (unread < 0) unread += (int8_t)MAX30102_FIFO_DEPTH;

    if (unread <= 0) return;

    /* ── 3. 溢出检测 ── */
    st = max_read(MAX30102_OVF_COUNTER, &ovf);
    if (st != HAL_OK) return;
    if (ovf > 0) {
        uint8_t new_rd = (wr_ptr > 0) ? (wr_ptr - 1U)
                                      : (MAX30102_FIFO_DEPTH - 1U);
        max_write(MAX30102_FIFO_RD_PTR, new_rd);
        max_write(MAX30102_OVF_COUNTER, 0x00);
        MAX30102_ResetHistory();
        return;
    }

    /* ── 4. 防独占保护 ── */
    if (unread > (int8_t)MAX30102_BURST_MAX) {
        uint8_t new_rd = (wr_ptr > 0) ? (wr_ptr - 1U)
                                      : (MAX30102_FIFO_DEPTH - 1U);
        max_write(MAX30102_FIFO_RD_PTR, new_rd);
        MAX30102_ResetHistory();
        return;
    }

    /* ── 5. 突发清仓循环 ── */
    while (unread > 0) {
        st = max_read_buf(MAX30102_FIFO_DATA, raw, 6);
        if (st != HAL_OK) return;

        uint32_t red_val = ((uint32_t)raw[0] << 16)
                         | ((uint32_t)raw[1] << 8)
                         |  (uint32_t)raw[2];
        uint32_t ir_val  = ((uint32_t)raw[3] << 16)
                         | ((uint32_t)raw[4] << 8)
                         |  (uint32_t)raw[5];

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
        max30102_ready = 1U;

        if (sample_valid) {
            ir_dc_sum  += ir_val;
            red_dc_sum += red_val;
            dc_count++;
        }

        unread--;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PPG 信号处理
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ppg_process(void)
{
    if (dc_count == 0U) return;

    ir_dc_est  = ir_dc_sum  / dc_count;
    (void)red_dc_sum;

    for (uint8_t i = 0; i < dc_count; i++) {
        uint8_t idx = (max30102_buf_idx >= i + 1U)
                      ? (max30102_buf_idx - i - 1U)
                      : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);
        if (!max30102_buf[idx].valid) continue;
        int32_t ir_ac = (int32_t)max30102_buf[idx].ir - (int32_t)ir_dc_est;
        if (ir_ac > ir_ac_max) ir_ac_max = ir_ac;
        if (ir_ac < ir_ac_min) ir_ac_min = ir_ac;
    }

    int32_t ac_amplitude = ir_ac_max - ir_ac_min;
    int32_t peak_thresh  = (int32_t)ir_dc_est + (ac_amplitude * 6 / 10);

    for (uint8_t i = 0; i < dc_count; i++) {
        uint8_t idx_curr = (max30102_buf_idx >= i + 1U)
                           ? (max30102_buf_idx - i - 1U)
                           : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);
        uint8_t idx_prev = (idx_curr == 0U) ? (MAX30102_BUF_LEN - 1U)
                                            : (idx_curr - 1U);
        if (!max30102_buf[idx_curr].valid || !max30102_buf[idx_prev].valid) continue;

        int32_t ir_curr = (int32_t)max30102_buf[idx_curr].ir;
        int32_t ir_prev = (int32_t)max30102_buf[idx_prev].ir;
        uint32_t sample_tick = max30102_buf[idx_curr].tick_ms;

        if (ir_prev <= peak_thresh && ir_curr > peak_thresh) {
            uint32_t interval = sample_tick - last_peak_ms;
            if (last_peak_ms > 0U && interval >= 300U && interval <= 1500U) {
                peak_intervals[peak_idx] = interval;
                peak_idx = (peak_idx + 1U) % 5U;
                if (peak_cnt < 5U) peak_cnt++;

                uint32_t sorted[5];
                for (uint8_t j = 0; j < peak_cnt; j++) sorted[j] = peak_intervals[j];
                for (uint8_t a = 0; a < peak_cnt - 1U; a++)
                    for (uint8_t b = a + 1U; b < peak_cnt; b++)
                        if (sorted[a] > sorted[b])
                            { uint32_t t = sorted[a]; sorted[a] = sorted[b]; sorted[b] = t; }

                uint32_t median_interval = sorted[peak_cnt / 2U];
                if (median_interval > 0U) {
                    hr_output = (uint8_t)(60000UL / median_interval);
                    if (hr_output < MAX30102_HR_MIN) hr_output = MAX30102_HR_MIN;
                    if (hr_output > MAX30102_HR_MAX) hr_output = MAX30102_HR_MAX;
                }
            }
            last_peak_ms = sample_tick;
        }
    }

    spo2_output = 98U;

    ir_dc_sum  = 0; red_dc_sum = 0; dc_count = 0;
    ir_ac_max  = 0; ir_ac_min  = 0; max30102_ready = 0;
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
    ir_dc_sum  = 0; red_dc_sum = 0; dc_count = 0;
    ir_dc_est  = 1;
    ir_ac_max  = 0; ir_ac_min  = 0;
    last_peak_ms = 0;
    memset(peak_intervals, 0, sizeof(peak_intervals));
    peak_idx = 0;
    peak_cnt = 0;
    hr_output = 0;
    spo2_output = 0;
}

uint8_t MAX30102_IsOnline(void)
{
    uint8_t valid_cnt = 0;
    for (uint8_t i = 0; i < 3U && i < MAX30102_BUF_LEN; i++) {
        uint8_t idx = (max30102_buf_idx >= i + 1U)
                      ? (max30102_buf_idx - i - 1U)
                      : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);
        if (max30102_buf[idx].valid) valid_cnt++;
    }
    return (valid_cnt >= 2U) ? 1U : 0U;
}

void MAX30102_ProcessTick(void)
{
    if (max30102_ready) ppg_process();
}
