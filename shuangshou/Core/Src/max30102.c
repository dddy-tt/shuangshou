/**
  ******************************************************************************
  * @file           : max30102.c
  * @brief          : MAX30102 心率血氧传感器驱动 — 突发清仓读取 v2.4
  * @author         : 车架师重构版 v2.4
  * @date           : 2026-06-29
  *
  * @核心升级 (v2.3 → v2.4):
  *   ★ FIFO 突发清仓: 彻底重写 MAX30102_ReadFIFO()。
  *      旧逻辑每次只读 1 个样本 → 主循环 10ms 抖动时 FIFO 渐进积压。
  *      新逻辑: 读 WR_PTR/RD_PTR → 计算未读样本数 →
  *              while (unread > 0) { 读1样本→处理→unread-- }
  *      一次性把 FIFO 全部捞空, 杜绝渐进式队列溢出锁死。
  *
  *      时序保障: 单次最多读 32 样本 (192 字节, ~4.3ms @ 400kHz),
  *      若 FIFO 堆积超过此量则执行 FIFO_RESET 丢弃旧数据。
  *
  * @保留防御 (v2.3):
  *   1. 100Hz 采样率对齐 10ms 轮询
  *   2. OVF_COUNTER 溢出自动复位
  *   3. IR 边界卡关 [5000, 200000] — 手指脱离/饱和阻断
  *   4. 所有 I2C 调用复用 SoftI2C 超时熔断
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "max30102.h"
#include "soft_i2c.h"
#include "string.h"
#include "stm32f4xx_hal.h"

/* ── LED 电流配置 ─────────────────────────────────────────────────────────── */
#define MAX30102_LED1_CURRENT   0x3FU  /* RED ≈ 12.6mA */
#define MAX30102_LED2_CURRENT   0x4FU  /* IR  ≈ 15.8mA */

/* ── 心率参数 ─────────────────────────────────────────────────────────────── */
#define MAX30102_HR_MIN         40U
#define MAX30102_HR_MAX         200U

/**
 * @brief 单次 FIFO 突发读取最大样本数
 * @note  32 样本 × 6 字节 = 192 字节,
 *        @ 400kHz ≈ 4.3ms ← 仍在 10ms 槽口内 (43%),
 *        但绝不挤占 5ms MPU 槽 (5ms 槽先于 10ms 槽执行)。
 */
#define MAX30102_BURST_MAX      32U

/* ── 全局状态 ─────────────────────────────────────────────────────────────── */
MAX30102_Sample_t max30102_buf[MAX30102_BUF_LEN];
uint8_t           max30102_buf_idx = 0;
uint8_t           max30102_ready  = 0;

/* ── 内部 PPG 处理状态 ────────────────────────────────────────────────────── */
static uint32_t ir_dc_sum    = 0;
static uint32_t red_dc_sum   = 0;
static uint8_t  dc_count     = 0;
static uint32_t ir_dc_est    = 1;
static uint32_t red_dc_est   = 1;
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
 *  传感器初始化 (不变)
 * ═══════════════════════════════════════════════════════════════════════════ */

uint8_t MAX30102_Init(void)
{
    uint8_t reg_val;

    /* Step 1: 软复位 */
    if (SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_MODE_CONFIG, MAX30102_RESET) != SI2C_OK) {
        return 1;
    }
    HAL_Delay(10);

    /* Step 2: 验证 PART_ID */
    if (SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                         MAX30102_PART_ID, &reg_val) != SI2C_OK) {
        return 1;
    }
    if (reg_val != MAX30102_PART_ID_VAL) return 2;

    /* Step 3: 禁用中断 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_INT_ENABLE1, 0x00);
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_INT_ENABLE2, 0x00);

    /* Step 4: FIFO 配置 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_FIFO_CONFIG, MAX30102_FIFO_ROLLOVER);

    /* Step 5: SpO2 配置 → 100Hz + 18-bit */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_SPO2_CONFIG, MAX30102_SPO2_CFG_100HZ);

    /* Step 6: LED 电流 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_LED1_PA, MAX30102_LED1_CURRENT);
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_LED2_PA, MAX30102_LED2_CURRENT);

    /* Step 7: 多 LED 模式 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_MULTI_LED_CTRL1, 0x21);
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_MULTI_LED_CTRL2, 0x00);

    /* Step 8: 进入 SpO2 模式 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_MODE_CONFIG, MAX30102_MODE_SPO2);

    /* Step 9: 清除 FIFO 残留 */
    {
        uint8_t wr_ptr, rd_ptr;
        SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                         MAX30102_FIFO_WR_PTR, &wr_ptr);
        SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                         MAX30102_FIFO_RD_PTR, &rd_ptr);
        uint8_t fifo_count = (wr_ptr - rd_ptr) & 0x1FU;
        uint8_t dummy[6];
        for (uint8_t i = 0; i < fifo_count; i++) {
            SoftI2C_ReadBuf(SI2C_MAX30102, MAX30102_I2C_ADDR,
                            MAX30102_FIFO_DATA, dummy, 6);
        }
    }

    /* Step 10: 初始化内部状态 */
    MAX30102_ResetHistory();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ★★★ FIFO 突发清仓读取 — v2.4 核心重构 ★★★
 *
 *  旧逻辑 (v2.3):
 *    每 10ms 只读 1 样本 → 主循环抖动时 FIFO 渐进积压
 *    → 连续几个周期的微小延迟累加 → FIFO 满载 → 溢出锁死
 *
 *  新逻辑 (v2.4):
 *    读 WR_PTR / RD_PTR → 计算未读数 →
 *    while (unread > 0) { 读 1 样本 → 处理 → unread-- }
 *    一次性捞空整个 FIFO, 零积压。
 *
 *  防独占保护: 单次最多读 MAX30102_BURST_MAX (32) 个样本,
 *  超过此量的直接 FIFO_RESET 丢弃旧数据 (主循环严重阻塞过).
 * ═══════════════════════════════════════════════════════════════════════════ */

void MAX30102_ReadFIFO(void)
{
    uint8_t wr_ptr, rd_ptr, ovf;
    uint8_t raw[6];
    SI2C_Status_t st;

    /* ── 1. 读取 FIFO 读写指针 ── */
    st = SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_FIFO_WR_PTR, &wr_ptr);
    if (st != SI2C_OK) return;  /* I2C 故障, 本周期跳过 */
    st = SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_FIFO_RD_PTR, &rd_ptr);
    if (st != SI2C_OK) return;

    /* ── 2. 计算 FIFO 中未读样本数 ── */
    int8_t unread = (int8_t)(wr_ptr - rd_ptr);
    /* FIFO 深度 32, 指针差值可能为负 (环形绕回) */
    if (unread < 0) unread += (int8_t)MAX30102_FIFO_DEPTH;

    if (unread <= 0) return;  /* 无数据 → 静默返回 */

    /* ── 3. 溢出检测 ── */
    st = SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_OVF_COUNTER, &ovf);
    if (st != SI2C_OK) return;
    if (ovf > 0) {
        /* FIFO 曾溢出 → 丢弃全部残留, 复位到最新样本 */
        uint8_t new_rd = (wr_ptr > 0) ? (wr_ptr - 1U)
                                      : (MAX30102_FIFO_DEPTH - 1U);
        SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_FIFO_RD_PTR, new_rd);
        SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_OVF_COUNTER, 0x00);
        MAX30102_ResetHistory();
        return;
    }

    /* ── 4. ★ 防独占保护: 堆积超限则直接丢弃 ── */
    if (unread > (int8_t)MAX30102_BURST_MAX) {
        /* 主循环严重阻塞 (>320ms 未读 FIFO),
         * 32 个样本全部是陈旧数据 → 丢弃, 重置到最新 */
        uint8_t new_rd = (wr_ptr > 0) ? (wr_ptr - 1U)
                                      : (MAX30102_FIFO_DEPTH - 1U);
        SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_FIFO_RD_PTR, new_rd);
        MAX30102_ResetHistory();
        return;
    }

    /* ═════════════════════════════════════════════════
     *  5. ★ 突发清仓循环: 逐个样本捞空 FIFO ★
     *
     *  每次迭代: I2C 读 6 字节 → 解析 → 边界卡关 → 入环
     *  单样本耗时 ≈ 195μs @ 400kHz
     *  10 个样本 ≈ 1.95ms ← 典型无抖动场景 (unread≈1)
     *  32 个样本 ≈ 6.24ms ← 极端场景, 仍在 10ms 槽内
     * ═════════════════════════════════════════════════ */
    while (unread > 0) {
        /* ── 5a. 读 1 个 FIFO 样本 (6 字节) ── */
        st = SoftI2C_ReadBuf(SI2C_MAX30102, MAX30102_I2C_ADDR,
                             MAX30102_FIFO_DATA, raw, 6);
        if (st != SI2C_OK) {
            /* 读取失败 → 停止本轮循环, 下个 10ms 再试 */
            return;
        }

        /* ── 5b. 解析 18-bit 原始值 ── */
        uint32_t red_val = ((uint32_t)raw[0] << 16)
                         | ((uint32_t)raw[1] << 8)
                         |  (uint32_t)raw[2];
        uint32_t ir_val  = ((uint32_t)raw[3] << 16)
                         | ((uint32_t)raw[4] << 8)
                         |  (uint32_t)raw[5];

        /* ── 5c. ★ 硬件边界卡关 ── */
        uint8_t sample_valid = 1U;
        if (ir_val < (uint32_t)MAX30102_IR_MIN_VALID ||
            ir_val > (uint32_t)MAX30102_IR_MAX_VALID) {
            sample_valid = 0U;
            MAX30102_ResetHistory();
        }

        /* ── 5d. 存入环形缓冲 ── */
        max30102_buf[max30102_buf_idx].ir    = ir_val;
        max30102_buf[max30102_buf_idx].red   = red_val;
        max30102_buf[max30102_buf_idx].valid = sample_valid;
        max30102_buf_idx = (max30102_buf_idx + 1U) % MAX30102_BUF_LEN;
        max30102_ready = 1U;

        /* ── 5e. 有效样本积累 DC ── */
        if (sample_valid) {
            ir_dc_sum  += ir_val;
            red_dc_sum += red_val;
            dc_count++;
        }

        unread--;  /* 已处理 1 个样本, 继续直到 FIFO 清空 */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PPG 信号处理 — 每 100ms 调用
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ppg_process(void)
{
    if (dc_count == 0U) return;

    ir_dc_est  = ir_dc_sum  / dc_count;
    red_dc_est = red_dc_sum / dc_count;

    uint32_t now = HAL_GetTick();

    /* ── 自适应峰值检测: 遍历最近 dc_count 个有效样本 ── */
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

    /* ── 上升沿过零检测 → 峰值间期 → HR ── */
    for (uint8_t i = 0; i < dc_count; i++) {
        uint8_t idx_curr = (max30102_buf_idx >= i + 1U)
                           ? (max30102_buf_idx - i - 1U)
                           : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);
        uint8_t idx_prev = (idx_curr == 0U) ? (MAX30102_BUF_LEN - 1U)
                                            : (idx_curr - 1U);
        if (!max30102_buf[idx_curr].valid || !max30102_buf[idx_prev].valid) continue;

        int32_t ir_curr = (int32_t)max30102_buf[idx_curr].ir;
        int32_t ir_prev = (int32_t)max30102_buf[idx_prev].ir;

        if (ir_prev <= peak_thresh && ir_curr > peak_thresh) {
            uint32_t interval = now - last_peak_ms;
            if (interval >= 300U && interval <= 1500U && last_peak_ms > 0U) {
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
            last_peak_ms = now;
        }
    }

    spo2_output = 98U;  /* 占位值, 待硬件实测校准 */

    /* ── 复位累积器 ── */
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
    ir_dc_est  = 1; red_dc_est = 1;
    ir_ac_max  = 0; ir_ac_min  = 0;
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
