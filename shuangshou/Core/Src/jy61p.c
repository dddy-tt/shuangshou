/**
  ******************************************************************************
  * @file           : jy61p.c
  * @brief          : 维特智能 JY61P 高精度 IMU — I2C Burst Read 驱动
  * @author         : 车架师重构版 v3.0
  * @date           : 2026-06-29
  *
  * @核心设计:
  *   1. Burst Read: 从 0x34 连读 12 字节 (ACC 6 + GYRO 6), 约 350μs
  *                  从 0x3D 连读  6 字节 (Roll+Pitch+Yaw), 约 210μs
  *   2. 超时熔断: 所有 I2C 调用均复用 SoftI2C_ReadBuf 的硬超时机制
  *   3. 单位转换: ACC  → m/s² (×4.785e-3), GYRO → °/s (×6.104e-2),
  *               ANGLE → °    (×5.493e-3)
  *
  * @与 MPU6050 的关键差异:
  *   - 不再需要互补滤波 (JY61P 内部卡尔曼已完成)
  *   - 不再需要陀螺零漂校准 (出厂已校准)
  *   - 新增 Yaw 轴 (MPU6050 因陀螺漂移无法获得可靠航向)
  *   - 角速度单位是 °/s (MPU6050 代码中是 rad/s), 迁移时注意!
  *   - I2C 地址从 0xD0 变成 0xA0
  *
  * @I2C 速度升级:
  *   SoftI2C_3 (MAX30102) 之前独占 400kHz,
  *   v3.0 将 MPU 通道也升到 400kHz (JY61P 明确支持 Fast Mode),
  *   利用 SoftI2C_ReadBuf 内部的 speed 自动选择逻辑。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "jy61p.h"
#include "soft_i2c.h"
#include "string.h"

/* ── 全局传感器数据 (各通道独立) ─────────────────────────────────────────── */
JY61P_Data_t JY61P_Right;  /* 右手 (通道1, SI2C_MPU_RIGHT) */
JY61P_Data_t JY61P_Left;   /* 左手 (通道2, SI2C_MPU_LEFT)  */

/* ── 通道号 → 软 I2C 设备编号映射 ── */
static SI2C_Dev_t channel_to_dev(uint8_t channel)
{
    return (channel == JY61P_CH_RIGHT) ? SI2C_MPU_RIGHT : SI2C_MPU_LEFT;
}

/* ── 两字节小端拼装为有符号 16-bit ── */
static int16_t bytes_to_s16(uint8_t lo, uint8_t hi)
{
    return (int16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}

/* ── 指向目标通道数据结构的指针 ── */
static JY61P_Data_t *data_of(uint8_t channel)
{
    return (channel == JY61P_CH_RIGHT) ? &JY61P_Right : &JY61P_Left;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  初始化 — 软复位 + 在线确认
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  JY61P 初始化: 传感器在线确认
 * @note
 *   Step 1: 尝试读一次欧拉角 (验证 I2C 通信)
 *   Step 2: 若失败 (传感器还在 UART 模式 / 排线断) → 标记离线
 *
 *   JY61P 无 WHO_AM_I 寄存器, 在线检测方式为:
 *     读取任意有效寄存器 → 若 SI2C_OK 则在线
 *
 * @warning 传感器必须在 PC 上位机软件中预先配置为 I2C 模式!
 *          出厂的 UART 模式无法通过 I2C 通信, 会直接返回 NACK。
 */
uint8_t JY61P_Init(uint8_t channel)
{
    SI2C_Dev_t dev = channel_to_dev(channel);
    JY61P_Data_t *d = data_of(channel);

    /* ── 清零数据结构 ── */
    memset(d, 0, sizeof(JY61P_Data_t));
    d->online = 0;

    /* ── 尝试读取一次 ANGLE 验证从机在线 ── */
    uint8_t buf[2];  /* 读 Roll 低字节 + 高字节 */
    SI2C_Status_t st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR,
                                        JY61P_REG_ROLL_L, buf, 2U);
    if (st != SI2C_OK) {
        /* 通信失败: 传感器不在线 / 仍是 UART 模式 / 排线断 */
        return 1;
    }

    d->online = 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ★ 核心: 全数据 Burst Read — 12 + 6 = 18 字节, 约 560μs
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  JY61P 全数据读取 — ACC(6B) + GYRO(6B) + ANGLE(6B)
 *
 * @时序分析 (SoftI2C @ 400kHz Fast Mode):
 *   ACC+GYRO Read: START + 2×AddrByte + REG + ReSTART + 12×DataByte + STOP
 *     ≈ 28 + 12×(9) ≈ 136 位 @ 400kHz → 136 × 2.5μs ≈ 340μs
 *   ANGLE Read:   START + 2×AddrByte + REG + ReSTART + 6×DataByte + STOP
 *     ≈ 28 + 6×(9) ≈ 82 位 @ 400kHz → 82 × 2.5μs ≈ 205μs
 *   总计 ≈ 545μs ★ 在 5ms 任务槽内 (10.9%)
 *
 * @note   参数可为 NULL 跳过对应数据段:
 *         JY61P_Read_Data(ch,  NULL, NULL, angle) — 只读角度
 *         JY61P_Read_Data(ch,  acc,  gyro, NULL)  — 只读惯导
 */
uint8_t JY61P_Read_Data(uint8_t channel, float *acc, float *gyro, float *angle)
{
    SI2C_Dev_t dev  = channel_to_dev(channel);
    JY61P_Data_t *d = data_of(channel);
    uint8_t buf[12];
    uint8_t err = 0;
    SI2C_Status_t st;

    /* ── 若之前已标记离线, 快速返回 ── */
    if (!d->online) {
        return 3;
    }

    /* ═════════════════════════════════════════════════
     *  阶段 1: Burst Read ACC (6B) + GYRO (6B) 从 0x34
     *  布局: AxL,AxH, AyL,AyH, AzL,AzH, GxL,GxH, GyL,GyH, GzL,GzH
     * ═════════════════════════════════════════════════ */
    st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR,
                         JY61P_REG_AX_L, buf, JY61P_BURST_ACC_GYRO_LEN);
    if (st != SI2C_OK) {
        d->online = 0;
        err |= 1;  /* ACC+GYRO 读取失败 */
        /* 不直接 return — 尝试继续读 ANGLE */
    } else {
        /* ── 解析 ACC (3 × int16, 小端) ── */
        d->acc_raw[0] = bytes_to_s16(buf[0], buf[1]);   /* Ax */
        d->acc_raw[1] = bytes_to_s16(buf[2], buf[3]);   /* Ay */
        d->acc_raw[2] = bytes_to_s16(buf[4], buf[5]);   /* Az */

        /* ── 解析 GYRO (3 × int16, 小端) ── */
        d->gyro_raw[0] = bytes_to_s16(buf[6], buf[7]);   /* Gx */
        d->gyro_raw[1] = bytes_to_s16(buf[8], buf[9]);   /* Gy */
        d->gyro_raw[2] = bytes_to_s16(buf[10], buf[11]); /* Gz */

        /* ── 转为物理量 ── */
        if (acc) {
            acc[0] = (float)d->acc_raw[0] * JY61P_ACC_SCALE;   /* m/s² */
            acc[1] = (float)d->acc_raw[1] * JY61P_ACC_SCALE;
            acc[2] = (float)d->acc_raw[2] * JY61P_ACC_SCALE;
        }
        if (gyro) {
            gyro[0] = (float)d->gyro_raw[0] * JY61P_GYRO_SCALE;  /* °/s */
            gyro[1] = (float)d->gyro_raw[1] * JY61P_GYRO_SCALE;
            gyro[2] = (float)d->gyro_raw[2] * JY61P_GYRO_SCALE;
        }

        /* ── 同步写入全局结构 ── */
        d->acc[0]  = (float)d->acc_raw[0]  * JY61P_ACC_SCALE;
        d->acc[1]  = (float)d->acc_raw[1]  * JY61P_ACC_SCALE;
        d->acc[2]  = (float)d->acc_raw[2]  * JY61P_ACC_SCALE;
        d->gyro[0] = (float)d->gyro_raw[0] * JY61P_GYRO_SCALE;
        d->gyro[1] = (float)d->gyro_raw[1] * JY61P_GYRO_SCALE;
        d->gyro[2] = (float)d->gyro_raw[2] * JY61P_GYRO_SCALE;
    }

    /* ═════════════════════════════════════════════════
     *  阶段 2: Burst Read ANGLE (6B) 从 0x3D
     *  布局: RollL,RollH, PitchL,PitchH, YawL,YawH
     * ═════════════════════════════════════════════════ */
    {
        uint8_t abuf[6];
        st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR,
                             JY61P_REG_ROLL_L, abuf, JY61P_BURST_ANGLE_LEN);
        if (st != SI2C_OK) {
            d->online = 0;
            err |= 2;  /* ANGLE 读取失败 */
        } else {
            d->angle_raw[0] = bytes_to_s16(abuf[0], abuf[1]);  /* Roll  */
            d->angle_raw[1] = bytes_to_s16(abuf[2], abuf[3]);  /* Pitch */
            d->angle_raw[2] = bytes_to_s16(abuf[4], abuf[5]);  /* Yaw   */

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

/**
 * @brief  仅读取欧拉角 — 用于只需姿态的场景 (跌倒检测/姿态冻结)
 * @note   数据源独立于 ACC/GYRO Burst Read, 不依赖 JY61P_Read_Data 调用
 */
uint8_t JY61P_Read_Angle(uint8_t channel, float *roll, float *pitch, float *yaw)
{
    SI2C_Dev_t dev  = channel_to_dev(channel);
    JY61P_Data_t *d = data_of(channel);
    uint8_t abuf[6];

    if (!d->online) return 1;

    SI2C_Status_t st = SoftI2C_ReadBuf(dev, JY61P_I2C_ADDR,
                                        JY61P_REG_ROLL_L, abuf, 6U);
    if (st != SI2C_OK) {
        d->online = 0;
        return 1;
    }

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

/* ═══════════════════════════════════════════════════════════════════════════
 *  辅助功能
 * ═══════════════════════════════════════════════════════════════════════════ */

uint8_t JY61P_IsOnline(uint8_t channel)
{
    return data_of(channel)->online;
}

/**
 * @brief  获取缓存的欧拉角 (不发起新的 I2C 事务, 零耗时)
 * @note   返回的是上一次 JY61P_Read_Data / JY61P_Read_Angle 的结果
 */
void JY61P_GetLastAngle(uint8_t channel, float *roll, float *pitch, float *yaw)
{
    JY61P_Data_t *d = data_of(channel);
    if (roll)  *roll  = d->angle[0];
    if (pitch) *pitch = d->angle[1];
    if (yaw)   *yaw   = d->angle[2];
}
