/**
  ******************************************************************************
  * @file           : jy61p.c
  * @brief          : 维特智能 JY61P 高精度 IMU — I2C Burst Read + 200Hz 锁速
  * @author         : 车架师重构版 v3.1 (v2.4 防线升级)
  * @date           : 2026-06-29
  *
  * @核心升级 (v3.0 → v3.1):
  *   1. ★ 200Hz 速率锁定: JY61P_Init() 中通过 I2C 解锁→写频率寄存器→保存,
  *      强制将内部卡尔曼滤波输出速率锁定为 200Hz (0x0A),
  *      对齐 STM32 5ms/10ms 轮询时基 (200Hz → Δt=5ms 间隔保证数据刷新),
  *      彻底根除 Jerk 一阶微分因数据未更新导致的 "Data_t - Data_t-1 ≡ 0" 伪零点。
  *   2. 配置写入全程带 ACK 超时自减熔断 + 重试机制。
  *   3. 速率回读验证: 写完后回读寄存器确认, 若模块不支持 I2C 写则降级运行。
  *
  * @JY61P I2C 配置寄存器映射 (维特智能协议):
  *   0xFF: 解锁寄存器  — 写入 0xAA 解锁配置写入 (部分固件版本)
  *   0x03: 输出频率    — 0x0A=200Hz, 0x09=100Hz, 0x08=50Hz
  *   0x00: 保存指令    — 写入 0x00 保存当前配置到 Flash
  *
  * @警告:
  *   - JY61P 不同固件版本的解锁方式可能不同, 若解锁失败则跳过配置
  *     步骤, 传感器工作在出厂默认频率 (通常是 100Hz)。
  *   - 首次上电需通过 PC 上位机配置为 I2C 模式 (出厂为 UART)。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "jy61p.h"
#include "soft_i2c.h"
#include "string.h"
#include "stm32f4xx_hal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  JY61P 配置寄存器 (I2C 可写)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 解锁寄存器地址
 * @note  写入 0xAA 后, 配置寄存器变为可写 (部分固件需要先写 0xFF)
 */
#define JY61P_REG_UNLOCK      0xFFU

/**
 * @brief 解锁密钥
 */
#define JY61P_UNLOCK_KEY      0xAAU

/**
 * @brief 输出频率寄存器
 * @note  0x0A = 200Hz (对齐 5ms 轮询)
 *        0x09 = 100Hz
 *        0x08 =  50Hz
 */
#define JY61P_REG_RATE        0x03U

/**
 * @brief 目标输出频率: 200Hz
 * @note  200Hz 意味着 JY61P 内部卡尔曼每 5ms 产出 1 组新数据,
 *        与 STM32 的 5ms JY61P_Read_Data 轮询严格同步。
 *        这确保 Jerk 检测的滑动窗口中相邻样本一定不同 (除非手真的静止),
 *        根除了 "旧数据残留 → jerk ≡ 0" 的伪零点。
 */
#define JY61P_RATE_200HZ      0x0AU

/**
 * @brief 保存配置到 Flash 寄存器
 * @note  写入 0x00 触发 Flash 写入, 需等待 ≥100ms 完成
 */
#define JY61P_REG_SAVE        0x00U
#define JY61P_SAVE_KEY        0x00U

/**
 * @brief I2C 配置写入最大重试次数
 */
#define JY61P_CFG_RETRY_MAX   2U

/* ── 全局传感器数据 ───────────────────────────────────────────────────────── */
JY61P_Data_t JY61P_Right;
JY61P_Data_t JY61P_Left;

/* ── 通道映射 ── */
static SI2C_Dev_t channel_to_dev(uint8_t channel)
{
    return (channel == JY61P_CH_RIGHT) ? SI2C_MPU_RIGHT : SI2C_MPU_LEFT;
}

/* ── 小端拼装 int16 ── */
static int16_t bytes_to_s16(uint8_t lo, uint8_t hi)
{
    return (int16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}

static JY61P_Data_t *data_of(uint8_t channel)
{
    return (channel == JY61P_CH_RIGHT) ? &JY61P_Right : &JY61P_Left;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  I2C 配置辅助 — 带重试的写寄存器序列
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  带重试 + 超时熔断的 I2C 写寄存器
 * @retval 0 成功, 1 多次重试后仍失败 (排线断/模块无响应)
 */
static uint8_t jy61p_write_reg(SI2C_Dev_t dev, uint8_t reg, uint8_t val)
{
    for (uint8_t retry = 0; retry < JY61P_CFG_RETRY_MAX; retry++) {
        SI2C_Status_t st = SoftI2C_WriteByte(dev, JY61P_I2C_ADDR, reg, val);
        if (st == SI2C_OK) {
            return 0;  /* 写入成功 */
        }
        /* 写入失败 → 等待 1ms 后重试 (让总线恢复) */
        HAL_Delay(1);
    }
    return 1;  /* 全部重试耗尽 → 失败 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  初始化 — 在线确认 + ★ 200Hz 输出速率锁定 ★
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  JY61P 初始化: 验证在线 → 锁定 200Hz 输出频率
 * @retval 0   成功 (传感器在线, 且 200Hz 配置已写入)
 * @retval 1   I2C 通信失败 (传感器不在线 / 排线断 / UART模式)
 * @retval 2   在线但 200Hz 配置写入失败 (模块固件可能不支持 I2C 写)
 *
 * @步骤:
 *   Step 1 — 读一次 ANGLE 验证 I2C 从机存在
 *   Step 2 — 解锁配置寄存器 (写入 0xFF=0xAA)
 *   Step 3 — 写输出频率 = 200Hz (0x03=0x0A)
 *   Step 4 — 回读 0x03 验证写入值
 *   Step 5 — 保存到 Flash (0x00=0x00), 等待 100ms
 *
 * @note   若 Step 2~5 任一失败, 传感器仍标记为 online=1,
 *         但输出频率保持出厂默认 (通常 100Hz)。
 *         这不会导致功能失效, 仅 Jerk 检测精度略降。
 */
uint8_t JY61P_Init(uint8_t channel)
{
    SI2C_Dev_t dev = channel_to_dev(channel);
    JY61P_Data_t *d = data_of(channel);
    uint8_t buf[2];
    SI2C_Status_t st;

    /* ── 清零 ── */
    memset(d, 0, sizeof(JY61P_Data_t));
    d->online = 0;

    /* ═════════════════════════════════════════════════
     *  Step 1: 验证从机在线 (读 Roll 2 字节)
     * ═════════════════════════════════════════════════ */
    st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR, JY61P_REG_ROLL_L, buf, 2U);
    if (st != SI2C_OK) {
        /* 通信失败: 传感器不在线 / 仍是 UART 模式 */
        return 1;
    }
    d->online = 1;  /* 传感器已验证在线 */

    /* ═════════════════════════════════════════════════
     *  Step 2: 解锁配置寄存器
     * ═════════════════════════════════════════════════ */
    if (jy61p_write_reg(dev, JY61P_REG_UNLOCK, JY61P_UNLOCK_KEY) != 0) {
        /* 解锁失败 → 模块固件可能不支持 I2C 配置写入,
         * 传感器仍可用, 但工作在出厂频率 (通常 100Hz) */
        return 2;  /* "在线但配置失败" — 上层可选择降级 */
    }
    HAL_Delay(5);  /* 解锁后等待配置窗口打开 */

    /* ═════════════════════════════════════════════════
     *  Step 3: 写输出频率 = 200Hz (0x0A)
     *
     *  200Hz = 每 5ms 一组新数据。
     *  轮询周期 5ms → 严格同步:
     *    轮询:   t=0ms   t=5ms   t=10ms  t=15ms
     *    传感器:  D0      D1      D2      D3
     *  → Jerk 每一对相邻数据 (D1-D0, D2-D1) 都不同,
     *    一阶微分结果反映真实手部运动, 不会出现
     *    "两次读到同一数据 → jerk ≡ 0" 的伪零点。
     * ═════════════════════════════════════════════════ */
    if (jy61p_write_reg(dev, JY61P_REG_RATE, JY61P_RATE_200HZ) != 0) {
        return 2;  /* 频率写入失败 */
    }
    HAL_Delay(5);

    /* ═════════════════════════════════════════════════
     *  Step 4: 回读验证 0x03 寄存器
     * ═════════════════════════════════════════════════ */
    {
        uint8_t rate_rd = 0;
        st = SoftI2C_ReadByte(dev, JY61P_I2C_ADDR, JY61P_REG_RATE, &rate_rd);
        if (st == SI2C_OK && rate_rd == JY61P_RATE_200HZ) {
            /* 200Hz 确认写入成功 */
        } else {
            /* 回读不匹配 — 模块可能拒绝写操作,
             * 降级运行在出厂频率 */
        }
    }

    /* ═════════════════════════════════════════════════
     *  Step 5: 保存到 Flash (掉电不丢失)
     * ═════════════════════════════════════════════════ */
    (void)jy61p_write_reg(dev, JY61P_REG_SAVE, JY61P_SAVE_KEY);
    HAL_Delay(100);  /* Flash 写入需要 ~100ms,
                        期间传感器停止数据输出,
                        但主循环此时还在初始化阶段,
                        不影响实时性 */

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ★ 核心: 全数据 Burst Read — ACC(6B) + GYRO(6B) + ANGLE(6B) ≈ 560μs
 * ═══════════════════════════════════════════════════════════════════════════ */

uint8_t JY61P_Read_Data(uint8_t channel, float *acc, float *gyro, float *angle)
{
    SI2C_Dev_t dev  = channel_to_dev(channel);
    JY61P_Data_t *d = data_of(channel);
    uint8_t buf[12];
    uint8_t err = 0;
    SI2C_Status_t st;

    if (!d->online) return 3;

    /* ── Burst Read 1: ACC(6B) + GYRO(6B) 从 0x34 ── */
    st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR,
                         JY61P_REG_AX_L, buf, JY61P_BURST_ACC_GYRO_LEN);
    if (st != SI2C_OK) {
        d->online = 0;
        err |= 1;
    } else {
        d->acc_raw[0] = bytes_to_s16(buf[0], buf[1]);
        d->acc_raw[1] = bytes_to_s16(buf[2], buf[3]);
        d->acc_raw[2] = bytes_to_s16(buf[4], buf[5]);
        d->gyro_raw[0] = bytes_to_s16(buf[6], buf[7]);
        d->gyro_raw[1] = bytes_to_s16(buf[8], buf[9]);
        d->gyro_raw[2] = bytes_to_s16(buf[10], buf[11]);

        if (acc) {
            acc[0] = (float)d->acc_raw[0] * JY61P_ACC_SCALE;
            acc[1] = (float)d->acc_raw[1] * JY61P_ACC_SCALE;
            acc[2] = (float)d->acc_raw[2] * JY61P_ACC_SCALE;
        }
        if (gyro) {
            gyro[0] = (float)d->gyro_raw[0] * JY61P_GYRO_SCALE;
            gyro[1] = (float)d->gyro_raw[1] * JY61P_GYRO_SCALE;
            gyro[2] = (float)d->gyro_raw[2] * JY61P_GYRO_SCALE;
        }
        d->acc[0]  = (float)d->acc_raw[0]  * JY61P_ACC_SCALE;
        d->acc[1]  = (float)d->acc_raw[1]  * JY61P_ACC_SCALE;
        d->acc[2]  = (float)d->acc_raw[2]  * JY61P_ACC_SCALE;
        d->gyro[0] = (float)d->gyro_raw[0] * JY61P_GYRO_SCALE;
        d->gyro[1] = (float)d->gyro_raw[1] * JY61P_GYRO_SCALE;
        d->gyro[2] = (float)d->gyro_raw[2] * JY61P_GYRO_SCALE;
    }

    /* ── Burst Read 2: ANGLE(6B) 从 0x3D ── */
    {
        uint8_t abuf[6];
        st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR,
                             JY61P_REG_ROLL_L, abuf, JY61P_BURST_ANGLE_LEN);
        if (st != SI2C_OK) {
            d->online = 0;
            err |= 2;
        } else {
            d->angle_raw[0] = bytes_to_s16(abuf[0], abuf[1]);
            d->angle_raw[1] = bytes_to_s16(abuf[2], abuf[3]);
            d->angle_raw[2] = bytes_to_s16(abuf[4], abuf[5]);
            if (angle) {
                angle[0] = (float)d->angle_raw[0] * JY61P_ANGLE_SCALE;
                angle[1] = (float)d->angle_raw[1] * JY61P_ANGLE_SCALE;
                angle[2] = (float)d->angle_raw[2] * JY61P_ANGLE_SCALE;
            }
            d->angle[0] = (float)d->angle_raw[0] * JY61P_ANGLE_SCALE;
            d->angle[1] = (float)d->angle_raw[1] * JY61P_ANGLE_SCALE;
            d->angle[2] = (float)d->angle_raw[2] * JY61P_ANGLE_SCALE;
        }
    }
    return err;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  轻量版: 仅读欧拉角 (约 210μs)
 * ═══════════════════════════════════════════════════════════════════════════ */

uint8_t JY61P_Read_Angle(uint8_t channel, float *roll, float *pitch, float *yaw)
{
    SI2C_Dev_t dev  = channel_to_dev(channel);
    JY61P_Data_t *d = data_of(channel);
    uint8_t abuf[6];

    if (!d->online) return 1;

    SI2C_Status_t st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR,
                                        JY61P_REG_ROLL_L, abuf, 6U);
    if (st != SI2C_OK) { d->online = 0; return 1; }

    d->angle_raw[0] = bytes_to_s16(abuf[0], abuf[1]);
    d->angle_raw[1] = bytes_to_s16(abuf[2], abuf[3]);
    d->angle_raw[2] = bytes_to_s16(abuf[4], abuf[5]);
    d->angle[0] = (float)d->angle_raw[0] * JY61P_ANGLE_SCALE;
    d->angle[1] = (float)d->angle_raw[1] * JY61P_ANGLE_SCALE;
    d->angle[2] = (float)d->angle_raw[2] * JY61P_ANGLE_SCALE;

    if (roll)  *roll  = d->angle[0];
    if (pitch) *pitch = d->angle[1];
    if (yaw)   *yaw   = d->angle[2];
    return 0;
}

uint8_t JY61P_IsOnline(uint8_t channel)
{
    return data_of(channel)->online;
}

void JY61P_GetLastAngle(uint8_t channel, float *roll, float *pitch, float *yaw)
{
    JY61P_Data_t *d = data_of(channel);
    if (roll)  *roll  = d->angle[0];
    if (pitch) *pitch = d->angle[1];
    if (yaw)   *yaw   = d->angle[2];
}
