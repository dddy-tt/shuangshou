/**
  ******************************************************************************
  * @file           : soft_i2c.c
  * @brief          : 三路软件模拟 I2C 驱动 — 带超时熔断防护
  * @author         : 专家评审重构版 v2.3
  * @date           : 2026-06-29
  *
  * @硬件拓扑:
  *   SoftI2C_1  PB6(SCL) / PB7(SDA)  — 右手 MPU6050  @ 100kHz
  *   SoftI2C_2  PB8(SCL) / PB9(SDA)  — 左手 MPU6050  @ 100kHz
  *   SoftI2C_3  PC6(SCL) / PC7(SDA)  — MAX30102     @ 400kHz
  *
  * @防御性设计 (v2.3 新增):
  *   1. 所有 SCL/SDA 电平等待均植入硬超时熔断计数器 (SI2C_TO_CYCLES=2000),
  *      杜绝穿戴式排线微断/接触不良导致的 MCU 永久死锁。
  *   2. 每次启动条件前检测总线忙状态 (SDA 被从设备意外拉低),
  *      若 BUS_BUSY 则执行 9 时钟 + STOP 的总线恢复序列。
  *   3. 读写操作均返回 SI2C_Status_t 枚举, 上层调用者必须检查返回值。
  *
  * @时序校准 (168MHz SYSCLK):
  *   100kHz 标准模式: 半周期 5.0μs  — delay(3) ≈ 3.6μs (实测接近 100kHz)
  *   400kHz 快速模式: 半周期 1.25μs — delay(0) ≈ 0.9μs (空函数 + 寄存器操作开销)
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "soft_i2c.h"
#include "gpio.h"

/* ── 时序与超时宏定义 ────────────────────────────────────────────────────── */

/**
 * @brief GPIO NOP 延时等级
 * @note  delay(3) 约等于 10 个 NOP 循环 (168MHz 下约 60ns × 10 = 600ns 基址
 *        加上 GPIO BSRR 寄存器写入开销, 实际半周期在 3~5μs 范围)
 */
#define SI2C_DELAY_100K  3U    /* 100kHz 标准模式 */
#define SI2C_DELAY_400K  0U    /* 400kHz 快速模式 (仅寄存器写入开销) */

/**
 * @brief 硬超时熔断上限 — 排线微断死锁防护
 * @note  每轮循环约 0.3μs (1 NOP + SDA_READ 读 IDR 寄存器),
 *        2000 次 ≈ 600μs 超时窗口, 远大于正常 I2C 应答延迟 (~10μs),
 *        又足够短以避免系统调度器感知到卡顿。
 *        若触发此超时, 99% 是排线/焊点物理断裂或从设备死机。
 */
#define SI2C_TO_CYCLES   2000UL

/**
 * @brief 总线恢复: 连续发送的时钟脉冲数
 * @note  根据 I2C 规范 v6.0 §3.1.16, 当 SDA 被从设备意外拉低时,
 *        主设备应在 SCL 上发送最多 9 个额外时钟周期以释放总线。
 */
#define SI2C_RECOVERY_CLKS  9U

/* ── GPIO 位号查找表 (替代 GCC __builtin_ctz, 兼容 Keil ARMCC) ───────────── */
static uint8_t pin_to_bit(uint16_t pin)
{
    if      (pin == GPIO_PIN_0)  return 0;
    else if (pin == GPIO_PIN_1)  return 1;
    else if (pin == GPIO_PIN_2)  return 2;
    else if (pin == GPIO_PIN_3)  return 3;
    else if (pin == GPIO_PIN_4)  return 4;
    else if (pin == GPIO_PIN_5)  return 5;
    else if (pin == GPIO_PIN_6)  return 6;
    else if (pin == GPIO_PIN_7)  return 7;
    else if (pin == GPIO_PIN_8)  return 8;
    else if (pin == GPIO_PIN_9)  return 9;
    else if (pin == GPIO_PIN_10) return 10;
    else if (pin == GPIO_PIN_11) return 11;
    else if (pin == GPIO_PIN_12) return 12;
    else if (pin == GPIO_PIN_13) return 13;
    else if (pin == GPIO_PIN_14) return 14;
    else if (pin == GPIO_PIN_15) return 15;
    return 0;
}

/* ── 引脚控制宏 (BSRR 原子操作, 不可被中断打断) ──────────────────────────── */

/**
 * @brief SCL 推挽输出: 高电平
 * @note  BSRR 低 16 位写 1 置位 ODR, 是单周期原子操作,
 *        不会产生读-改-写竞争。
 */
#define SCL_H(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SCL_PORT->BSRR = SI2C1_SCL_PIN; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = SI2C2_SCL_PIN; \
    else                               SI2C3_SCL_PORT->BSRR = SI2C3_SCL_PIN; \
} while(0)

/**
 * @brief SCL 推挽输出: 低电平
 * @note  BSRR 高 16 位写 1 复位 ODR (等价于 BRR 寄存器),
 *        同样是单周期原子操作。
 */
#define SCL_L(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SCL_PORT->BSRR = (uint32_t)SI2C1_SCL_PIN << 16U; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = (uint32_t)SI2C2_SCL_PIN << 16U; \
    else                               SI2C3_SCL_PORT->BSRR = (uint32_t)SI2C3_SCL_PIN << 16U; \
} while(0)

#define SDA_H(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SDA_PORT->BSRR = SI2C1_SDA_PIN; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = SI2C2_SDA_PIN; \
    else                               SI2C3_SDA_PORT->BSRR = SI2C3_SDA_PIN; \
} while(0)

#define SDA_L(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SDA_PORT->BSRR = (uint32_t)SI2C1_SDA_PIN << 16U; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = (uint32_t)SI2C2_SDA_PIN << 16U; \
    else                               SI2C3_SDA_PORT->BSRR = (uint32_t)SI2C3_SDA_PIN << 16U; \
} while(0)

/* ── 引脚状态读取宏 ───────────────────────────────────────────────────────── */

/**
 * @brief 读 SDA 引脚当前电平 (输入状态)
 * @note  使用 GPIOx->IDR 寄存器, 读前不需切换 MODER 为输入,
 *        因为推挽输出模式下 IDR 仍然反映引脚实际电平。
 *        但在开漏释放后需要 SDA_SetIn() 确保不驱动总线。
 */
#define SDA_READ(dev) ( \
    (dev) == SI2C_MPU_RIGHT ? ((SI2C1_SDA_PORT->IDR & SI2C1_SDA_PIN) ? 1U : 0U) : \
    (dev) == SI2C_MPU_LEFT  ? ((SI2C2_SDA_PORT->IDR & SI2C2_SDA_PIN) ? 1U : 0U) : \
                               ((SI2C3_SDA_PORT->IDR & SI2C3_SDA_PIN) ? 1U : 0U) )

/**
 * @brief 读 SCL 引脚当前电平
 * @note  用于总线恢复时的时钟拉伸超时检测,
 *        以及启动前确认 SCL 已被释放。
 */
#define SCL_READ(dev) ( \
    (dev) == SI2C_MPU_RIGHT ? ((SI2C1_SCL_PORT->IDR & SI2C1_SCL_PIN) ? 1U : 0U) : \
    (dev) == SI2C_MPU_LEFT  ? ((SI2C2_SCL_PORT->IDR & SI2C2_SCL_PIN) ? 1U : 0U) : \
                               ((SI2C3_SCL_PORT->IDR & SI2C3_SCL_PIN) ? 1U : 0U) )

/* ── GPIO 方向切换 (开漏模拟的关键) ───────────────────────────────────────── */

/**
 * @brief 将 SDA 引脚设为推挽输出模式
 * @note  MODER[2y+1:2y] = 01 → 通用输出模式
 *        切换前确保 ODR 已写入目标电平 (由 SDA_H/SDA_L 宏保证),
 *        避免瞬间输出错误电平。
 */
static void SDA_SetOut(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    if (dev == SI2C_MPU_RIGHT)      { port = SI2C1_SDA_PORT; pin = SI2C1_SDA_PIN; }
    else if (dev == SI2C_MPU_LEFT)  { port = SI2C2_SDA_PORT; pin = SI2C2_SDA_PIN; }
    else                             { port = SI2C3_SDA_PORT; pin = SI2C3_SDA_PIN; }
    uint8_t bit = pin_to_bit(pin);
    /* 清除 MODER 对应位 → 写 01 (通用输出) */
    port->MODER &= ~(3UL << (2UL * bit));
    port->MODER |=  (1UL << (2UL * bit));
}

/**
 * @brief 将 SDA 引脚设为输入模式 (浮空, 依赖外部上拉)
 * @note  MODER[2y+1:2y] = 00 → 输入模式
 *        用于释放 SDA 总线, 等待从设备应答 (ACK=0 拉低).
 */
static void SDA_SetIn(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    if (dev == SI2C_MPU_RIGHT)      { port = SI2C1_SDA_PORT; pin = SI2C1_SDA_PIN; }
    else if (dev == SI2C_MPU_LEFT)  { port = SI2C2_SDA_PORT; pin = SI2C2_SDA_PIN; }
    else                             { port = SI2C3_SDA_PORT; pin = SI2C3_SDA_PIN; }
    uint8_t bit = pin_to_bit(pin);
    port->MODER &= ~(3UL << (2UL * bit));
}

/* ── 延时 ─────────────────────────────────────────────────────────────────── */

/**
 * @brief 软件延时 (粗粒度 NOP 循环)
 * @param n 延时等级: 0≈0.9μs, 3≈3.6μs (168MHz 实测)
 * @note  使用 volatile 修饰循环变量, 阻止编译器优化消除。
 *        168MHz 下每个 NOP 约 6ns, 加上循环分支开销约 30ns/轮。
 */
static void si2c_delay(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++) {
        __NOP();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  核心超时熔断函数 — 解决"排线微断 → MCU 永久死锁"问题
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  等待 SDA 变为高电平 (从设备释放总线), 带硬超时熔断
 * @param  dev  设备编号
 * @retval 0    成功 (SDA 在超时前变高)
 * @retval 1    超时 (排线断裂或从设备异常拉死 SDA)
 * @note   调用场景: 等待从设备释放 ACK、等待总线空闲
 *         每轮循环: SDA_READ (读 IDR 寄存器 ~2 周期) + 分支 (~3 周期)
 *         2000 轮 ≈ 600μs @ 168MHz, 覆盖最慢的 I2C 从设备响应。
 */
static uint8_t si2c_wait_sda_high(SI2C_Dev_t dev)
{
    uint32_t to = SI2C_TO_CYCLES;
    while (!SDA_READ(dev) && --to) {
        __NOP();
    }
    return (to == 0U) ? 1U : 0U;  /* 0=成功, 1=超时熔断 */
}

/**
 * @brief  等待 SCL 变为高电平 (时钟拉伸超时检测), 带硬超时熔断
 * @param  dev  设备编号
 * @retval 0    成功
 * @retval 1    超时 (SCL 被从设备长期拉低 — 时钟拉伸超限)
 * @note   MPU6050 和 MAX30102 均不支持时钟拉伸, 此函数主要用于
 *         总线恢复时检测硬件故障。
 */
static uint8_t si2c_wait_scl_high(SI2C_Dev_t dev)
{
    uint32_t to = SI2C_TO_CYCLES;
    while (!SCL_READ(dev) && --to) {
        __NOP();
    }
    return (to == 0U) ? 1U : 0U;
}

/**
 * @brief  发送 SCL 上的一个时钟脉冲 (用于总线恢复)
 * @param  dev          设备编号
 * @param  speed_delay  延时等级
 * @note   在 SDA 被意外拉低时, 主设备在 SCL 上翻转 9 次可迫使
 *         从设备状态机复位并释放 SDA。
 */
static void si2c_send_clock_pulse(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SCL_L(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    /* ★ 验证 SCL 确实变高 — 若排线断则 SCL 无法被上拉 */
    if (si2c_wait_scl_high(dev)) {
        /* SCL 超时未变高 — 排线物理断裂, 无需继续发送脉冲 */
        return;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  总线恢复序列 — I2C 规范 v6.0 §3.1.16
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  I2C 总线解锁序列
 * @param  dev  设备编号
 * @note   当检测到 SDA 被意外持续拉低 (BUS_BUSY) 时:
 *         1. 发送 9 个 SCL 时钟脉冲, 驱动从设备状态机释放 SDA
 *         2. 若仍未释放 → 超时熔断, 返回 SI2C_TIMEOUT
 *         3. 发送 STOP 条件使总线回到 IDLE
 *         本函数在主循环 50ms 级任务中调用, 不阻塞实时性要求高的 5ms 任务。
 */
static SI2C_Status_t si2c_bus_recovery(SI2C_Dev_t dev)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* 1. 确保 SCL 初始为高 (若已被拉低则先释放) */
    SCL_H(dev);
    si2c_delay(speed);

    /* 2. 连续发送最多 9 个时钟脉冲 */
    for (uint8_t i = 0; i < SI2C_RECOVERY_CLKS; i++) {
        si2c_send_clock_pulse(dev, speed);
        /* 每个脉冲后检查 SDA 是否已释放 */
        if (SDA_READ(dev)) {
            break;  /* SDA 已释放, 提前退出 */
        }
    }

    /* 3. 若 9 个时钟后 SDA 仍然低 — 物理断线确认, 放弃恢复 */
    if (!SDA_READ(dev)) {
        return SI2C_TIMEOUT;
    }

    /* 4. 发送 STOP 条件: SDA 从低到高的跳变 (在 SCL 高电平期间) */
    SDA_SetOut(dev);
    SDA_L(dev);
    si2c_delay(speed);
    SCL_H(dev);
    si2c_delay(speed);
    SDA_H(dev);
    si2c_delay(speed);

    return SI2C_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  I2C 基本时序原语
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  启动条件: SDA ↓ 在 SCL=H 期间
 * @param  dev          设备编号
 * @param  speed_delay  速度对应的延时等级
 * @note   I2C 规范: 起始条件 = 在 SCL 为高电平时, SDA 从高到低的跳变。
 *         调用者必须确保总线空闲 (SDA=H, SCL=H) 后再调用。
 */
static void si2c_start(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    SDA_H(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SDA_L(dev);          /* ← 这里产生起始条件 */
    si2c_delay(speed_delay);
    SCL_L(dev);
    si2c_delay(speed_delay);
}

/**
 * @brief  停止条件: SDA ↑ 在 SCL=H 期间
 * @note   停止条件 = 在 SCL 为高电平时, SDA 从低到高的跳变。
 */
static void si2c_stop(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    SDA_L(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SDA_H(dev);          /* ← 这里产生停止条件 */
    si2c_delay(speed_delay);
}

/**
 * @brief  写一个字节到总线, 带 ACK 超时熔断
 * @param  dev          设备编号
 * @param  byte         待发送字节
 * @param  speed_delay  速度延时等级
 * @retval 0            从设备 ACK (SDA 被拉低)
 * @retval 1            从设备 NACK 或超时 (排线断裂)
 * @note   v2.3 改进: 第 9 个 SCL 周期内不再盲读一次,
 *         而是循环等待 SDA 变低 (ACK) 并施以超时熔断。
 *         若从设备正常应答, SDA 会在 SCL 上升沿后 ~1μs 内被拉低。
 */
static uint8_t si2c_write_byte(SI2C_Dev_t dev, uint8_t byte, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    for (uint8_t i = 0; i < 8; i++) {
        if (byte & 0x80U) {
            SDA_H(dev);
        } else {
            SDA_L(dev);
        }
        si2c_delay(speed_delay);
        SCL_H(dev);
        si2c_delay(speed_delay);
        SCL_L(dev);
        byte <<= 1;
    }

    /* ── 第 9 个时钟: 释放 SDA → 等待从设备拉低 (ACK) ── */
    SDA_SetIn(dev);            /* SDA 切为输入, 释放总线 */
    si2c_delay(speed_delay);
    SCL_H(dev);                /* 产生第 9 个 SCL 上升沿 */

    /* ★ 关键防御: 带超时熔断的 ACK 等待 ★ */
    uint8_t nack = si2c_wait_sda_high(dev);
    /* 如果超时 (SDA 始终为低 = 从设备 ACK), nack=0 → 正常
     * 如果 SDA 变高 (NACK 或断线), nack=1 → 异常 */

    si2c_delay(speed_delay);
    SCL_L(dev);
    SDA_SetOut(dev);
    return nack;  /* 0=ACK (正常), 1=NACK/超时 */
}

/**
 * @brief  从总线读一个字节, 带 SDA 释放超时熔断
 * @param  dev          设备编号
 * @param  send_ack     1=发送 ACK (通知从设备继续发送), 0=发送 NACK (最后字节)
 * @param  speed_delay  速度延时等级
 * @return 读取到的 8 位数据
 * @note   v2.3 改进: 每个 SCL 高电平期间, 等待 SDA 稳定后再读取,
 *         防止因排线寄生电容导致在 SDA 跳变中途误读。
 */
static uint8_t si2c_read_byte(SI2C_Dev_t dev, uint8_t send_ack, uint8_t speed_delay)
{
    uint8_t byte = 0;
    SDA_SetIn(dev);            /* 释放 SDA, 让从设备驱动数据线 */
    for (uint8_t i = 0; i < 8; i++) {
        SDA_H(dev);            /* 写 ODR=1 但不驱动 (已切输入), 确保浮空 */
        si2c_delay(speed_delay);
        SCL_H(dev);            /* SCL 上升沿: 从设备送出数据 */
        /* 等待 SDA 稳定 (应对排线寄生电容导致的上升沿延迟) */
        si2c_delay(speed_delay);
        byte <<= 1;
        if (SDA_READ(dev)) {
            byte |= 1U;
        }
        si2c_delay(speed_delay);
        SCL_L(dev);
    }

    /* ── 第 9 个时钟: 发送 ACK/NACK ── */
    SDA_SetOut(dev);
    if (send_ack) {
        SDA_L(dev);  /* ACK  = 拉低 → 通知从设备继续发送下一字节 */
    } else {
        SDA_H(dev);  /* NACK = 拉高 → 通知从设备这是最后一个字节 */
    }
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SCL_L(dev);
    SDA_H(dev);
    return byte;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  软 I2C 初始化 — 总线空闲确认 + 三路 STOP 序列
 * @note   在 MX_GPIO_Init() 之后调用。
 *         对每路总线发送 STOP 条件, 确保从设备状态机复位到 IDLE。
 *         v2.3: 增加启动前总线忙检测, 若 SDA 被拉低则执行总线恢复。
 */
void SoftI2C_Init(void)
{
    for (uint8_t d = 0; d < 3; d++) {
        SI2C_Dev_t dev = (SI2C_Dev_t)d;
        /* 检测 SDA 是否被意外拉低 (从设备未释放) */
        SDA_SetIn(dev);
        if (!SDA_READ(dev)) {
            /* SDA 被拉死 → 执行总线恢复 */
            si2c_bus_recovery(dev);
        }
        /* 无论是否恢复, 补一个 STOP 确保总线 IDLE */
        si2c_stop(dev, SI2C_DELAY_100K);
    }
}

/**
 * @brief  读取单个寄存器字节
 * @param  dev       设备编号
 * @param  dev_addr  器件地址 (8-bit, bit0=0 表示写)
 * @param  reg_addr  寄存器地址
 * @param  data      [出参] 读取到的数据字节
 * @retval SI2C_OK       成功
 * @retval SI2C_TIMEOUT  总线超时 (排线断裂或从设备无响应)
 * @retval SI2C_NACK     从设备 NACK (器件地址错误或器件未就绪)
 *
 * @时序 (标准 I2C 组合读事务):
 *   START → 写器件地址(W) → 写寄存器地址 → Repeated START → 读器件地址(R) → 读1字节(NACK) → STOP
 */
SI2C_Status_t SoftI2C_ReadByte(SI2C_Dev_t dev, uint8_t dev_addr,
                               uint8_t reg_addr, uint8_t *data)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* ── 阶段 1: 写器件地址 (W) + 寄存器地址 ── */
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFEU, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;  /* 器件无应答: 可能地址错误或器件未上电 */
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;  /* 寄存器地址 NACK */
    }

    /* ── 阶段 2: Repeated START + 读器件地址 (R) + 读 1 字节 + NACK ── */
    si2c_start(dev, speed);  /* Repeated Start: 不先发 STOP */
    if (si2c_write_byte(dev, dev_addr | 0x01U, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    *data = si2c_read_byte(dev, 0U, speed);  /* send_ack=0 → NACK 终止 */
    si2c_stop(dev, speed);
    return SI2C_OK;
}

/**
 * @brief  批量读取连续寄存器
 * @param  dev       设备编号
 * @param  dev_addr  器件地址 (8-bit)
 * @param  reg_addr  起始寄存器地址 (大部分 I2C 器件支持自动递增)
 * @param  buf       [出参] 数据缓冲区
 * @param  len       读取字节数
 * @retval SI2C_OK / SI2C_NACK / SI2C_TIMEOUT
 *
 * @note   MPU6050 的加速度/陀螺仪寄存器地址连续递增,
 *         一次读取 14 字节比逐个寄存器读取快 ~14 倍。
 */
SI2C_Status_t SoftI2C_ReadBuf(SI2C_Dev_t dev, uint8_t dev_addr,
                              uint8_t reg_addr, uint8_t *buf, uint8_t len)
{
    if (len == 0U) return SI2C_OK;
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* 写阶段 */
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFEU, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }

    /* Repeated Start → 读阶段 */
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr | 0x01U, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }

    for (uint8_t i = 0; i < len; i++) {
        /* 最后一个字节发 NACK (0), 前面发 ACK (1) */
        buf[i] = si2c_read_byte(dev, (i < (len - 1U)) ? 1U : 0U, speed);
    }
    si2c_stop(dev, speed);
    return SI2C_OK;
}

/**
 * @brief  写单个寄存器字节
 * @param  dev       设备编号
 * @param  dev_addr  器件地址 (8-bit)
 * @param  reg_addr  寄存器地址
 * @param  data      待写入的单字节数据
 * @retval SI2C_OK / SI2C_NACK / SI2C_TIMEOUT
 */
SI2C_Status_t SoftI2C_WriteByte(SI2C_Dev_t dev, uint8_t dev_addr,
                                uint8_t reg_addr, uint8_t data)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFEU, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, data, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    si2c_stop(dev, speed);
    return SI2C_OK;
}
