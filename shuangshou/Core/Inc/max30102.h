/**
  ******************************************************************************
  * @file           : max30102.h
  * @brief          : MAX30102 心率血氧传感器驱动 — 头文件
  * @author         : 专家评审重构版 v2.3
  * @date           : 2026-06-29
  *
  * @硬件连接:
  *   SoftI2C_3  PC6(SCL) / PC7(SDA)  — MAX30102  @ 400kHz Fast Mode
  *   电源: 独立 LC 滤波分支 (10μH + 100μF) → AMS1117(#2) → 3.3V
  *
  * @关键约束:
  *   1. 采样率严格对齐主循环 10ms (100Hz) 轮询周期
  *   2. 每次读取前必须检查 FIFO_NUM_SAMPLES 和溢出标志
  *   3. 原始数据需经过手指脱离/饱和边界卡关 (5000~200000)
  ******************************************************************************
  */

#ifndef __MAX30102_H
#define __MAX30102_H

#include "stdint.h"

/* ── 器件地址 ─────────────────────────────────────────────────────────────── */
#define MAX30102_I2C_ADDR       0xAEU  /* 7-bit: 0x57, 8-bit 写地址: 0xAE */
#define MAX30102_PART_ID_VAL    0x15U  /* PART_ID 寄存器期望值 */

/* ── 寄存器映射 ───────────────────────────────────────────────────────────── */
#define MAX30102_INT_STATUS1    0x00U  /* 中断状态 1 */
#define MAX30102_INT_STATUS2    0x01U  /* 中断状态 2 */
#define MAX30102_INT_ENABLE1    0x02U  /* 中断使能 1 */
#define MAX30102_INT_ENABLE2    0x03U  /* 中断使能 2 */
#define MAX30102_FIFO_WR_PTR    0x04U  /* FIFO 写指针 */
#define MAX30102_OVF_COUNTER    0x05U  /* FIFO 溢出计数器 */
#define MAX30102_FIFO_RD_PTR    0x06U  /* FIFO 读指针 */
#define MAX30102_FIFO_DATA      0x07U  /* FIFO 数据寄存器 (连续6字节) */
#define MAX30102_FIFO_CONFIG    0x08U  /* FIFO 配置 */
#define MAX30102_MODE_CONFIG    0x09U  /* 模式配置 */
#define MAX30102_SPO2_CONFIG    0x0AU  /* SpO2 配置 (采样率 + LED 脉宽) */
#define MAX30102_LED1_PA        0x0CU  /* LED1 (RED) 脉冲幅度 */
#define MAX30102_LED2_PA        0x0DU  /* LED2 (IR)  脉冲幅度 */
#define MAX30102_PILOT_PA       0x10U  /* Proximity 模式 LED 幅度 */
#define MAX30102_MULTI_LED_CTRL1 0x11U /* 多 LED 模式控制 1 */
#define MAX30102_MULTI_LED_CTRL2 0x12U /* 多 LED 模式控制 2 */
#define MAX30102_TEMP_INTEGER   0x1FU  /* 温度整数部分 */
#define MAX30102_TEMP_FRACTION  0x20U  /* 温度小数部分 (精度 0.0625°C) */
#define MAX30102_TEMP_CONFIG    0x21U  /* 温度传感器使能 */
#define MAX30102_REV_ID         0xFEU  /* 修订版本号 */
#define MAX30102_PART_ID        0xFFU  /* 器件 ID (应为 0x15) */

/* ── 模式配置寄存器位定义 ─────────────────────────────────────────────────── */
#define MAX30102_MODE_HR        0x02U  /* 仅心率模式 (RED only) */
#define MAX30102_MODE_SPO2      0x03U  /* SpO2 模式 (RED + IR) */
#define MAX30102_MODE_MULTI     0x07U  /* 多 LED 模式 */
#define MAX30102_SHDN           0x80U  /* 关断模式 (写入此位进入 0μA 待机) */
#define MAX30102_RESET          0x40U  /* 软复位 */

/* ── SpO2 配置寄存器位定义 (100Hz 对齐) ───────────────────────────────────── */
/* ADC 分辨率: 15 bits, 采样率 100Hz, LED 脉宽 411μs (18-bit) */
/* 组合: SPO2_SR[4:2]=011 (100Hz), SPO2_LED_PW[1:0]=11 (411μs, 18-bit ADC) */
#define MAX30102_SPO2_CFG_100HZ 0x1FU  /* SR=100Hz, PW=411μs/18bit */

/* ── FIFO 配置 ────────────────────────────────────────────────────────────── */
#define MAX30102_FIFO_ROLLOVER  0x10U  /* FIFO 满后覆盖旧数据 (不清空) */
#define MAX30102_FIFO_AEMPTY(n) ((n) & 0x0FU) /* Almost Empty 阈值 */
#define MAX30102_FIFO_DEPTH     32U    /* MAX30102 内部 FIFO 深度 32 样本 */
#define MAX30102_FIFO_SAMPLE_BYTES 6U  /* 单样本: RED[2]+IR[2] = 6 字节 */

/* ── PPG 数据质量边界卡关阈值 ─────────────────────────────────────────────── */
/**
 * @brief 红外通道 (IR) 原始计数有效范围
 * @note  低于 5000: 手指完全脱离传感器, 光电管无反射信号 → 视为开路
 *        高于 200000: 红外接收管饱和 (强环境光直射 或 LED 电流过高) → 数据无效
 *        范围内:   正常 PPG 波形 (指尖毛细血管搏动信号在 8000~150000 之间)
 */
#define MAX30102_IR_MIN_VALID   5000L
#define MAX30102_IR_MAX_VALID   200000L

/* ── PPG 数据缓冲 ─────────────────────────────────────────────────────────── */
#define MAX30102_BUF_LEN        20U   /* 缓冲最近 20 个样本 (200ms 窗口) */

/* ── 单样本数据结构 ───────────────────────────────────────────────────────── */
typedef struct {
    uint32_t ir;      /* 红外通道原始计数 (18-bit, 0~262143) */
    uint32_t red;     /* 红光通道原始计数 (18-bit) */
    uint8_t  valid;   /* 1=数据有效, 0=手指脱离/饱和 */
} MAX30102_Sample_t;

/* ── 全局状态 ──────────────────────────────────────────────────────────────── */
extern MAX30102_Sample_t max30102_buf[MAX30102_BUF_LEN];
extern uint8_t           max30102_buf_idx;
extern uint8_t           max30102_ready;   /* 缓冲区有 ≥1 个新样本时置 1 */

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief  MAX30102 初始化序列
 * @retval 0   成功
 * @retval 1   I2C 通信失败 (排线断或器件不存在)
 * @retval 2   器件 ID 不匹配 (非 MAX30102)
 * @note   执行: 软复位 → 等待 → 验证 PART_ID → 配置 SpO2 模式 → 100Hz 采样率
 *         → FIFO 配置 → LED 电流设置 → 清除 FIFO
 *         采样率强制设为 100Hz, 与主循环 10ms 轮询严格同频,
 *         避免 FIFO 过快堆积或过慢产生阶梯波形。
 */
uint8_t MAX30102_Init(void);

/**
 * @brief  FIFO 分时读取: 每次读取 1 个样本 (约 195μs @ 400kHz)
 * @note   由主循环 10ms 任务调用 (100Hz 同频轮询)
 *         流程: 读 FIFO_NUM_SAMPLES → 溢出检测 → 读 6 字节 → 边界卡关
 *         若发生溢出, 自动执行 FIFO_RESET 清除残留。
 */
void MAX30102_ReadFIFO(void);

/**
 * @brief  获取心率估计值 (bpm)
 * @retval 心率值 (40~200 bpm), 0=数据无效
 * @note   基于峰值间期法: HR = 60 / ΔT_peak
 *         需在 100ms 任务中调用, 此时缓冲区累积 ≥10 个新样本。
 */
uint8_t MAX30102_GetHR(void);

/**
 * @brief  获取血氧估计值 (%)
 * @retval SpO2 值 (80~100%), 0=数据无效
 * @note   SpO2 ≈ 104 - 17 × R_ratio
 *         R_ratio = (AC_red / DC_red) / (AC_ir / DC_ir)
 */
uint8_t MAX30102_GetSpO2(void);

/**
 * @brief  强制清除 PPG 滤波历史
 * @note   当检测到手指脱离或硬件饱和时调用,
 *         防止自适应峰值检测器在无效数据上继续运算。
 */
void MAX30102_ResetHistory(void);

/**
 * @brief  查询传感器是否在线 (手指是否贴合)
 * @retval 1  传感器在线且数据有效
 * @retval 0  传感器离线 (手指脱离 / 排线断 / 硬件饱和)
 */
uint8_t MAX30102_IsOnline(void);

/**
 * @brief  PPG 批量处理入口 (由主循环 100ms 任务调用)
 * @note   内部调用 ppg_process(): DC 均值估算 → 峰值检测 → HR/SpO2 输出
 *         调用后通过 MAX30102_GetHR() / MAX30102_GetSpO2() 获取结果
 */
void MAX30102_ProcessTick(void);

#endif /* __MAX30102_H */
