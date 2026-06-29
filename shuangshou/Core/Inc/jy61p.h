/**
  ******************************************************************************
  * @file           : jy61p.h
  * @brief          : 维特智能 JY61P 高精度 IMU 驱动 — I2C 模式 (头文件)
  * @author         : 车架师重构版 v3.0
  * @date           : 2026-06-29
  *
  * @硬件连接:
  *   右手 JY61P: SoftI2C_1  PB6(SCL) / PB7(SDA)  @ 400kHz
  *   左手 JY61P: SoftI2C_2  PB8(SCL) / PB9(SDA)  @ 400kHz
  *
  * @升级要点 (v2.3 MPU6050 → v3.0 JY61P):
  *   1. 零硬件改线: JY61P 的 SCL/SDA 直替原 MPU6050 的 SCL/SDA 排线位置
  *   2. 数据质量跃升: 卡尔曼滤波输出, 无重力分量泄露, 无需互补滤波
  *   3. 新增 Yaw 轴: 可检测水平面旋转手势 (之前因陀螺漂移无法获取)
  *   4. 寄存器连读: 12 字节 ACC+GYRO Burst Read, 约 350μs @ 400kHz
  *
  * @I2C 协议:
  *   7-bit 地址: 0x50 → 8-bit 写地址 0xA0, 读地址 0xA1
  *   数据格式: 16-bit 有符号整数 (小端, Data_L 在低地址)
  *   量程: 加速度 ±16g, 角速度 ±2000°/s, 角度 ±180°
  ******************************************************************************
  */

#ifndef __JY61P_H
#define __JY61P_H

#include "stdint.h"

/* ── I2C 地址 ─────────────────────────────────────────────────────────────── */
#define JY61P_I2C_ADDR    0xA0U   /* 8-bit 写地址 (7-bit: 0x50 << 1) */

/* ── I2C 通道编号 ─────────────────────────────────────────────────────────── */
#define JY61P_CH_RIGHT    1U      /* 右手: SoftI2C_1 → SI2C_MPU_RIGHT */
#define JY61P_CH_LEFT     2U      /* 左手: SoftI2C_2 → SI2C_MPU_LEFT  */

/* ═══════════════════════════════════════════════════════════════════════════
 *  寄存器映射 (JY61P I2C Mode)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 加速度起始寄存器
 * @note  0x34(AxL)~0x39(AzH), 6 字节连续, 每轴 16-bit 小端
 *        量程 ±16g, 分辨率 1/32768 × 16g
 */
#define JY61P_REG_AX_L      0x34U

/**
 * @brief 角速度起始寄存器
 * @note  0x3A(GxL)~0x3F(GzH), 6 字节连续, 每轴 16-bit 小端
 *        量程 ±2000°/s, 分辨率 1/32768 × 2000°/s
 */
#define JY61P_REG_GX_L      0x3AU

/**
 * @brief 欧拉角起始寄存器
 * @note  0x3D(RollL)~0x42(YawH), 6 字节连续, 每轴 16-bit 小端
 *        量程 ±180°, 分辨率 1/32768 × 180°
 *
 *        寄存器详细布局:
 *          0x3D: Roll  Data_L  (bit7..0)
 *          0x3E: Roll  Data_H  (bit15..8)
 *          0x3F: Pitch Data_L
 *          0x40: Pitch Data_H
 *          0x41: Yaw   Data_L
 *          0x42: Yaw   Data_H
 *
 * @warning 顺序是 Roll → Pitch → Yaw, 不是常见的 Pitch → Roll → Yaw
 */
#define JY61P_REG_ROLL_L    0x3DU

/* ═══════════════════════════════════════════════════════════════════════════
 *  连读 (Burst Read) 字节数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 从 0x34 开始连续读 12 字节 → ACC(6) + GYRO(6)
 * @note  耗时约 350μs @ 400kHz 软 I2C, 远在 5ms 任务片以内
 *
 *        Byte[0..1]:  Ax (0x34-0x35)
 *        Byte[2..3]:  Ay (0x36-0x37)
 *        Byte[4..5]:  Az (0x38-0x39)
 *        Byte[6..7]:  Gx (0x3A-0x3B)
 *        Byte[8..9]:  Gy (0x3C-0x3D)
 *        Byte[10..11]: Gz (0x3E-0x3F)
 */
#define JY61P_BURST_ACC_GYRO_LEN  12U

/**
 * @brief 从 0x3D 开始连续读 6 字节 → Roll + Pitch + Yaw
 * @note  耗时约 210μs @ 400kHz
 */
#define JY61P_BURST_ANGLE_LEN      6U

/* ═══════════════════════════════════════════════════════════════════════════
 *  物理量转换常数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 加速度: raw / 32768 × 16g × 9.8 m/s²/g
 *        即 raw × (16.0 × 9.8 / 32768) = raw × 0.00478515625
 *        预乘 1000000 用整数算, 最后除回去: raw × 4785 / 1000000
 */
#define JY61P_ACC_SCALE      0.00478515625f  /* 16g × 9.8 / 32768 */

/**
 * @brief 角速度: raw / 32768 × 2000°/s
 *        即 raw × (2000.0 / 32768) = raw × 0.06103515625
 */
#define JY61P_GYRO_SCALE     0.06103515625f  /* 2000 / 32768 */

/**
 * @brief 欧拉角: raw / 32768 × 180°
 *        即 raw × (180.0 / 32768) = raw × 0.0054931640625
 */
#define JY61P_ANGLE_SCALE    0.0054931640625f /* 180 / 32768 */

/* ═══════════════════════════════════════════════════════════════════════════
 *  数据结构
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 单路 JY61P 完整数据集
 * @note  acc[3]: Ax, Ay, Az (m/s²)
 *        gyro[3]: Gx, Gy, Gz (°/s)
 *        angle[3]: Roll, Pitch, Yaw (°)
 *        online: 1=传感器在线且数据有效, 0=通信失败
 */
typedef struct {
    float    acc[3];       /* 加速度 m/s² */
    float    gyro[3];      /* 角速度 °/s (注意: 非 rad/s!) */
    float    angle[3];     /* 欧拉角 ° [Roll, Pitch, Yaw] */
    int16_t  acc_raw[3];   /* 加速度原始 LSB (调试用) */
    int16_t  gyro_raw[3];  /* 角速度原始 LSB (调试用) */
    int16_t  angle_raw[3]; /* 角度原始 LSB (调试用) */
    uint8_t  online;       /* 1=本次读取成功 */
} JY61P_Data_t;

/**
 * @brief 全局双路传感器数据
 */
extern JY61P_Data_t JY61P_Right;  /* 右手 (通道1) */
extern JY61P_Data_t JY61P_Left;   /* 左手 (通道2) */

/* ═══════════════════════════════════════════════════════════════════════════
 *  API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  JY61P 初始化 (验证从机在线)
 * @param  channel  JY61P_CH_RIGHT (1) 或 JY61P_CH_LEFT (2)
 * @retval 0   成功, 传感器在线
 * @retval 1   I2C 通信超时 (排线断 / 传感器未上电 / 仍是 UART 模式)
 * @retval 2   软复位无应答
 * @note   调用时机: main() 中 SoftI2C_Init() 之后
 *         初始化序列: 软复位 → 延迟 → 验证 I2C 通信 → 记录在线状态
 */
uint8_t JY61P_Init(uint8_t channel);

/**
 * @brief  ★ 核心函数: Burst Read 12 字节 ACC+GYRO + 6 字节 ANGLE
 * @param  channel  JY61P_CH_RIGHT / JY61P_CH_LEFT
 * @param  acc      [出参] acc[0..2] = Ax, Ay, Az (m/s²), 可为 NULL 跳过
 * @param  gyro     [出参] gyro[0..2] = Gx, Gy, Gz (°/s), 可为 NULL 跳过
 * @param  angle    [出参] angle[0..2] = Roll, Pitch, Yaw (°), 可为 NULL 跳过
 * @retval 0   全部读取成功
 * @retval 1   ACC+GYRO Burst Read 超时/NACK (排线断)
 * @retval 2   ANGLE Burst Read 超时/NACK
 * @retval 3   全部失败
 *
 * @note   ★ 时序保障:
 *        ACC+GYRO 连读 12 字节 ≈ 350μs @ 400kHz
 *        ANGLE   连读  6 字节 ≈ 210μs @ 400kHz
 *        总计 ≈ 560μs, 在 5ms 任务槽口内 (占比 11.2%)
 *
 *        ★ 调用范例:
 *        float acc[3], gyro[3], angle[3];
 *        JY61P_Read_Data(JY61P_CH_RIGHT, acc, gyro, angle);
 *        // 直接使用 angle[0](Roll), angle[1](Pitch), angle[2](Yaw)
 *        // 不再需要互补滤波、零漂校准!
 */
uint8_t JY61P_Read_Data(uint8_t channel, float *acc, float *gyro, float *angle);

/**
 * @brief  仅读取欧拉角 (轻量版, 约 210μs)
 * @param  channel  通道号
 * @param  roll     [出参] Roll °
 * @param  pitch    [出参] Pitch °
 * @param  yaw      [出参] Yaw °
 * @retval 0  成功, 1  失败
 * @note   用于只需姿态角的场景 (如跌倒检测)
 */
uint8_t JY61P_Read_Angle(uint8_t channel, float *roll, float *pitch, float *yaw);

/**
 * @brief  查询传感器在线状态
 * @retval 1  在线, 0  离线
 */
uint8_t JY61P_IsOnline(uint8_t channel);

/**
 * @brief  获取上次读取的欧拉角 (不发起新 I2C 事务)
 * @note   仅返回上一次 JY61P_Read_Data 缓存的值
 */
void JY61P_GetLastAngle(uint8_t channel, float *roll, float *pitch, float *yaw);

#endif /* __JY61P_H */
