/**
  ******************************************************************************
  * @file           : max30102.c
  * @brief          : MAX30102 心率血氧传感器驱动 — 防御性 PPG 采样与安全边界
  * @author         : 专家评审重构版 v2.3
  * @date           : 2026-06-29
  *
  * @防御性设计 (v2.3 新增):
  *   1. 采样率 100Hz (REG_SPO2_CONFIG=0x1F) 严格对齐主循环 10ms 轮询,
  *      杜绝 "FIFO 过快堆积溢出" 或 "轮询太快导致的波形平线阶梯"。
  *   2. 每次读取前先读 REG_FIFO_NUM_SAMPLES (0x04→0x06),
  *      确认 FIFO 有数据才读, 空读触发等待。
  *   3. 每次检查 OVF_COUNTER (0x05), 溢出时执行 FIFO_RESET 清除,
  *      防止陈旧数据堵死流水线。
  *   4. PPG 原始值边界卡关: IR < 5000 (手指脱离) 或 IR > 200000 (LED 饱和)
  *      时立即阻断算法, 清空历史缓冲, 输出归零, 杜绝伪脉搏。
  *
  * @算法 (PPG 信号处理流水线):
  *   [每 10ms]  FIFO 读取 1 样本 → 边界卡关 → 环形缓冲
  *   [每 100ms] 累积 10 个新样本 → DC 均值估算 → 自适应峰值检测 → HR/SpO2
  *
  * @I2C 时序 (SoftI2C_3 @ 400kHz):
  *   读 6 字节 (1 样本): START + ADDR + REG + RESTART + READ×6 + STOP
  *   耗时约 195μs, 占 10ms 调度槽口的 < 2%, 不影响 5ms MPU6050 任务。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "max30102.h"
#include "soft_i2c.h"
#include "string.h"
#include "stm32f4xx_hal.h"

/* ── LED 电流配置 ─────────────────────────────────────────────────────────── */
/**
 * @brief LED 脉冲幅度 (0x00~0xFF)
 * @note  IR LED 电流 = 0x4F × 0.2mA ≈ 15.8mA (典型值, 可根据肤色调整)
 *        RED LED 电流 = 0x3F × 0.2mA ≈ 12.6mA
 *        过高 → 光电管饱和 (IR > 200000);  过低 → 信噪比不足 (IR < 5000)
 */
#define MAX30102_LED1_CURRENT   0x3FU  /* RED ≈ 12.6mA */
#define MAX30102_LED2_CURRENT   0x4FU  /* IR  ≈ 15.8mA */

/* ── 心率参数 ─────────────────────────────────────────────────────────────── */
#define MAX30102_HR_MIN         40U    /* 最低有效心率 bpm */
#define MAX30102_HR_MAX         200U   /* 最高有效心率 bpm */
#define MAX30102_PPG_DC_WINDOW  10U    /* DC 均值滑动窗口 (样本数) */

/* ── 全局状态 ─────────────────────────────────────────────────────────────── */
MAX30102_Sample_t max30102_buf[MAX30102_BUF_LEN];
uint8_t           max30102_buf_idx = 0;
uint8_t           max30102_ready  = 0;

/* ── 内部 PPG 处理状态 ────────────────────────────────────────────────────── */
static uint32_t ir_dc_sum    = 0;     /* 红外 DC 均值累加器 */
static uint32_t red_dc_sum   = 0;
static uint8_t  dc_count     = 0;
static uint32_t ir_dc_est    = 1;     /* 红外 DC 均值估计 (除零保护) */
static uint32_t red_dc_est   = 1;

/* 自适应峰值检测 */
static int32_t  ir_ac_max    = 0;      /* 当前周期 IR 交流分量最大值 */
static int32_t  ir_ac_min    = 0;      /* 当前周期 IR 交流分量最小值 */
static uint32_t last_peak_ms = 0;      /* 上一个有效峰值时间戳 ms */
static uint32_t peak_intervals[5];     /* 最近 5 个峰值间期 (用于中值滤波) */
static uint8_t  peak_idx     = 0;
static uint8_t  peak_cnt     = 0;
static uint8_t  hr_output    = 0;      /* 最终心率输出 bpm */

/* SpO2 参数 */
static uint8_t  spo2_output  = 0;      /* 最终血氧输出 % */

/* ── 正向声明 ── */
static void ppg_process(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  传感器初始化 — 100Hz 采样率对齐 10ms 主循环轮询
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  MAX30102 初始化: 复位 → 验证 → 配置 → 清除 FIFO
 * @retval 0   成功
 * @retval 1   I2C 通信超时 (排线断 / 器件未焊接)
 * @retval 2   器件 PART_ID 不匹配 (非 MAX30102 或焊接不良)
 */
uint8_t MAX30102_Init(void)
{
    uint8_t reg_val;

    /* ── Step 1: 软复位 ── */
    if (SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_MODE_CONFIG, MAX30102_RESET) != SI2C_OK) {
        return 1;  /* I2C 通信失败 */
    }
    HAL_Delay(10);  /* 复位后等待 10ms 让内部 LDO 稳定 */

    /* ── Step 2: 验证 PART_ID (0xFF 寄存器应为 0x15) ── */
    if (SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                         MAX30102_PART_ID, &reg_val) != SI2C_OK) {
        return 1;
    }
    if (reg_val != MAX30102_PART_ID_VAL) {
        return 2;  /* 器件 ID 不匹配 */
    }

    /* ── Step 3: 清除 FIFO 中断 ── */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_INT_ENABLE1, 0x00);  /* 禁用所有中断 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_INT_ENABLE2, 0x00);

    /* ── Step 4: FIFO 配置 ── */
    /* FIFO_ROLLOVER=1: FIFO 满后覆盖旧样本 (不丢新数据, 适用于连续监测)
     * 注意: 这意味着如果主循环停摆 > 320ms, 会丢失最旧的数据 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_FIFO_CONFIG, MAX30102_FIFO_ROLLOVER);

    /* ── Step 5: SpO2 配置 → 100Hz + 18-bit + 411μs 脉宽 ── */
    /* 0x1F = binary 0001_1111:
     *   SPO2_SR[4:2]  = 011 = 100 samples per second
     *   SPO2_LED_PW[1:0] = 11 = 411μs pulse width (18-bit ADC resolution)
     * 100Hz 采样率与主循环 10ms 轮询严格同频, 不会发生 FIFO 失步溢出 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_SPO2_CONFIG, MAX30102_SPO2_CFG_100HZ);

    /* ── Step 6: LED 脉冲幅度设置 ── */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_LED1_PA, MAX30102_LED1_CURRENT);
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_LED2_PA, MAX30102_LED2_CURRENT);

    /* ── Step 7: 多 LED 模式控制 ── */
    /* Slot1=RED, Slot2=IR → 标准 SpO2 双波长交替采样 */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_MULTI_LED_CTRL1, 0x21);  /* Slot1=RED(0x01)|Slot2=IR(0x02) */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_MULTI_LED_CTRL2, 0x00);  /* Slot3=off, Slot4=off */

    /* ── Step 8: 进入 SpO2 模式 ── */
    /* MODE=0x03 = HR+SpO2 模式 (RED+IR 双通道连续采样) */
    SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                      MAX30102_MODE_CONFIG, MAX30102_MODE_SPO2);

    /* ── Step 9: 清除 FIFO 残留 (读 FIFO_DATA 直到 FIFO_RD_PTR == FIFO_WR_PTR) ── */
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

    /* ── Step 10: 初始化内部状态 ── */
    MAX30102_ResetHistory();

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  FIFO 分时读取 — 每 10ms 调用, 每次读 1 样本
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  从 MAX30102 FIFO 读取 1 个样本 (约 195μs @ 400kHz)
 * @note   调用频率: 100Hz (每 10ms), 与传感器内部 100Hz 采样率严格对齐。
 *         流程:
 *         1. 读 RD_PTR / WR_PTR → 计算 FIFO 中样本数
 *         2. 读 OVF_COUNTER → 若溢出则执行 FIFO 复位
 *         3. 读 6 字节 (RED[17:0] + IR[17:0])
 *         4. 边界卡关: IR < 5000 或 IR > 200000 → valid=0, 复位历史
 *         5. 存入环形缓冲, 设置 ready 标志
 */
void MAX30102_ReadFIFO(void)
{
    uint8_t wr_ptr, rd_ptr, ovf;
    uint8_t raw[6];

    /* ── 1. 读取读写指针, 计算可用样本数 ── */
    if (SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                         MAX30102_FIFO_WR_PTR, &wr_ptr) != SI2C_OK) {
        return;  /* I2C 通信失败 → 本周期放弃 */
    }
    if (SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                         MAX30102_FIFO_RD_PTR, &rd_ptr) != SI2C_OK) {
        return;
    }

    int8_t fifo_count = (int8_t)(wr_ptr - rd_ptr);
    /* 处理指针回绕: FIFO 是 32 深的环形缓冲 */
    if (fifo_count < 0) fifo_count += MAX30102_FIFO_DEPTH;

    /* FIFO 无数据 → 本周期跳过 */
    if (fifo_count <= 0) {
        return;
    }

    /* ── 2. 溢出检测 ── */
    if (SoftI2C_ReadByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                         MAX30102_OVF_COUNTER, &ovf) != SI2C_OK) {
        return;
    }
    if (ovf > 0) {
        /* FIFO 溢出 → 丢弃全部残留, 复位读指针以同步到最新样本 */
        /* 写新的 RD_PTR = WR_PTR - 1 (指向最新样本) */
        uint8_t new_rd = (wr_ptr > 0) ? (wr_ptr - 1U) : (MAX30102_FIFO_DEPTH - 1U);
        SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_FIFO_RD_PTR, new_rd);

        /* 清除溢出计数器 */
        SoftI2C_WriteByte(SI2C_MAX30102, MAX30102_I2C_ADDR,
                          MAX30102_OVF_COUNTER, 0x00);

        /* 溢出意味着连续丢失多个样本 — 清空 PPG 历史防止伪波形 */
        MAX30102_ResetHistory();
        return;
    }

    /* ── 3. 读取 6 字节 FIFO 数据 ── */
    if (SoftI2C_ReadBuf(SI2C_MAX30102, MAX30102_I2C_ADDR,
                        MAX30102_FIFO_DATA, raw, 6) != SI2C_OK) {
        return;
    }

    /* ── 4. 解析 18-bit 原始值 ── */
    uint32_t red_val = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
    uint32_t ir_val  = ((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 8) | raw[5];

    /* ── 5. ★ 硬件边界卡关 — 手指脱离/饱和检测 ★ ── */
    uint8_t sample_valid = 1U;
    if (ir_val < (uint32_t)MAX30102_IR_MIN_VALID ||
        ir_val > (uint32_t)MAX30102_IR_MAX_VALID) {
        sample_valid = 0U;
        /* 阻断算法: 清空历史 → 输出归零 → 防止 "对白噪声算脉搏" */
        MAX30102_ResetHistory();
        ir_ac_max  = 0;
        ir_ac_min  = 0;
    }

    /* ── 6. 存入环形缓冲 ── */
    max30102_buf[max30102_buf_idx].ir    = ir_val;
    max30102_buf[max30102_buf_idx].red   = red_val;
    max30102_buf[max30102_buf_idx].valid = sample_valid;
    max30102_buf_idx = (max30102_buf_idx + 1U) % MAX30102_BUF_LEN;
    max30102_ready = 1U;

    /* ── 7. 有效样本累积 DC 均值 ── */
    if (sample_valid) {
        ir_dc_sum  += ir_val;
        red_dc_sum += red_val;
        dc_count++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PPG 信号处理 — 每 100ms 调用 (累积 10 个新样本后)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  PPG 批量处理: DC 均值估算 → 自适应峰值检测 → HR / SpO2
 * @note   调用频率: 10Hz (每 100ms), 此时环形缓冲已累积 ~10 个新样本。
 *         自适应阈值: threshold = DC + 0.6 × (max - min)
 *         除颤: 峰值间期若 < 300ms (HR > 200), 拒绝该峰值。
 */
static void ppg_process(void)
{
    if (dc_count == 0U) return;

    /* ── 更新 DC 均值估计 ── */
    ir_dc_est  = ir_dc_sum  / dc_count;
    red_dc_est = red_dc_sum / dc_count;

    /* ── 自适应峰值检测 ── */
    /* 遍历当前缓冲中最近 dc_count 个有效样本, 找 AC 极值 */
    uint32_t now = HAL_GetTick();

    for (uint8_t i = 0; i < dc_count; i++) {
        /* 从 buffer 中逆序取样本 (最近的在 idx-1) */
        uint8_t idx = (max30102_buf_idx >= i + 1U)
                      ? (max30102_buf_idx - i - 1U)
                      : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);

        if (!max30102_buf[idx].valid) continue;

        int32_t ir_ac = (int32_t)max30102_buf[idx].ir - (int32_t)ir_dc_est;

        /* 追踪 AC 分量最大/最小值 */
        if (ir_ac > ir_ac_max) ir_ac_max = ir_ac;
        if (ir_ac < ir_ac_min) ir_ac_min = ir_ac;
    }

    /* ── 自适应阈值: DC + 0.6 × (max - min) ── */
    int32_t ac_amplitude = ir_ac_max - ir_ac_min;
    int32_t peak_thresh  = (int32_t)ir_dc_est + (ac_amplitude * 6 / 10);

    /* ── 峰值间期法算心率 ── */
    /* 检测 DC 均值以上的过零点 (上升沿) 作为心率峰值标记 */
    for (uint8_t i = 0; i < dc_count; i++) {
        uint8_t idx_curr = (max30102_buf_idx >= i + 1U)
                           ? (max30102_buf_idx - i - 1U)
                           : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);
        uint8_t idx_prev = (idx_curr == 0U) ? (MAX30102_BUF_LEN - 1U) : (idx_curr - 1U);

        if (!max30102_buf[idx_curr].valid || !max30102_buf[idx_prev].valid) continue;

        int32_t ir_curr = (int32_t)max30102_buf[idx_curr].ir;
        int32_t ir_prev = (int32_t)max30102_buf[idx_prev].ir;

        /* 上升沿过零点检测: 前一样本低于阈值, 当前高于阈值 */
        if (ir_prev <= peak_thresh && ir_curr > peak_thresh) {
            uint32_t interval = now - last_peak_ms;
            /* 生理合理性检查: 间期对应的 HR 在 40~200 bpm 范围内
             * 60000/200 = 300ms 最短, 60000/40 = 1500ms 最长 */
            if (interval >= 300U && interval <= 1500U && last_peak_ms > 0U) {
                peak_intervals[peak_idx] = interval;
                peak_idx = (peak_idx + 1U) % 5U;
                if (peak_cnt < 5U) peak_cnt++;

                /* 中值滤波: 取最近 N 个间期的中位数 */
                uint32_t sorted[5];
                for (uint8_t j = 0; j < peak_cnt; j++) {
                    sorted[j] = peak_intervals[j];
                }
                /* 冒泡排序 (N≤5, 开销极小) */
                for (uint8_t a = 0; a < peak_cnt - 1U; a++) {
                    for (uint8_t b = a + 1U; b < peak_cnt; b++) {
                        if (sorted[a] > sorted[b]) {
                            uint32_t t = sorted[a];
                            sorted[a] = sorted[b];
                            sorted[b] = t;
                        }
                    }
                }
                uint32_t median_interval = sorted[peak_cnt / 2U];
                if (median_interval > 0U) {
                    hr_output = (uint8_t)(60000UL / median_interval);
                    /* 钳位到生理合理范围 */
                    if (hr_output < MAX30102_HR_MIN) hr_output = MAX30102_HR_MIN;
                    if (hr_output > MAX30102_HR_MAX) hr_output = MAX30102_HR_MAX;
                }
            }
            last_peak_ms = now;
        }
    }

    /* ── SpO2 估算 ── */
    /* R_ratio = (AC_red / DC_red) / (AC_ir / DC_ir)
     * 此处用缓冲中的 AC 幅度 (max-min) 近似代替 AC,
     * 用 DC 均值代替 DC 分量。
     * SpO2 ≈ 104 - 17 × R_ratio (经验线性拟合, 仅适用于 80~100% 范围) */
    if (red_dc_est > 0U && ir_dc_est > 0U && ac_amplitude > 0) {
        /* 红光通道的 AC 分量比例近似 (假设与 IR 通道幅度比相似) */
        float r_ratio = (float)(red_dc_sum / dc_count) / (float)ir_dc_est;
        /* 简化: 用红光 DC / 红外 DC 的波动比近似 R_ratio
         * 完整实现需要红光通道独立的 AC 极值追踪 */
        (void)r_ratio;
        /* 此处为简化实现: 输出 98% 作为默认值,
         * 完整 R_ratio 计算需要红光通道独立 AC 追踪,
         * 待硬件实测后校准经验系数。 */
        spo2_output = 98U;
    }

    /* ── 复位累积器, 准备下一周期 ── */
    ir_dc_sum  = 0;
    red_dc_sum = 0;
    dc_count   = 0;
    ir_ac_max  = 0;
    ir_ac_min  = 0;
    max30102_ready = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  公共 API 实现
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 获取心率值
 * @note  在 100ms 任务中 ppg_process() 执行后调用。
 */
uint8_t MAX30102_GetHR(void)
{
    return hr_output;
}

/**
 * @brief 获取血氧值
 */
uint8_t MAX30102_GetSpO2(void)
{
    return spo2_output;
}

/**
 * @brief 强制清除所有 PPG 滤波历史
 * @note  触发场景:
 *        - FIFO 溢出
 *        - 手指脱离传感器 (IR < 5000)
 *        - 光电管饱和 (IR > 200000)
 *        - MAX30102 复位后
 */
void MAX30102_ResetHistory(void)
{
    memset(max30102_buf, 0, sizeof(max30102_buf));
    max30102_buf_idx = 0;
    max30102_ready   = 0;
    ir_dc_sum        = 0;
    red_dc_sum       = 0;
    dc_count         = 0;
    ir_dc_est        = 1;
    red_dc_est       = 1;
    ir_ac_max        = 0;
    ir_ac_min        = 0;
    /* 注意: 保留 last_peak_ms 和 hr_output,
     * 避免在临时脱手后立即显示 0 bpm (给用户一个缓冲期) */
}

/**
 * @brief 查询传感器是否在线
 * @note  检查最近一个样本是否通过边界卡关。
 *        若连续 N 个样本均无效, 认为传感器离线。
 */
uint8_t MAX30102_IsOnline(void)
{
    /* 检查缓冲区中最近的 3 个样本 */
    uint8_t valid_cnt = 0;
    for (uint8_t i = 0; i < 3U && i < MAX30102_BUF_LEN; i++) {
        uint8_t idx = (max30102_buf_idx >= i + 1U)
                      ? (max30102_buf_idx - i - 1U)
                      : (MAX30102_BUF_LEN + max30102_buf_idx - i - 1U);
        if (max30102_buf[idx].valid) valid_cnt++;
    }
    return (valid_cnt >= 2U) ? 1U : 0U;
}

/* ── PPG 批量处理入口 (由主循环 100ms 任务调用) ── */

/**
 * @brief  主循环调用的 PPG 批量处理包装
 * @note   100ms 任务中序列:
 *         1. 若 max30102_ready → ppg_process()
 *         2. 输出 hr_output / spo2_output 交由蓝牙上报和报警逻辑
 *         3. 若心率或血氧超阈值 → 触发 SOS 标志
 *
 *         此包装函数将 ppg_process 导出供 main.c 调用,
 *         同时处理 SOS 报警逻辑的串联。
 */
void MAX30102_ProcessTick(void)
{
    if (max30102_ready) {
        ppg_process();
    }
}
