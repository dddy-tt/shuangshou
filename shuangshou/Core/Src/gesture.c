/**
  ******************************************************************************
  * @file           : gesture.c
  * @brief          : 双手手势识别引擎 — 手形编码 + 门限微分法 + 防御性保护
  * @author         : 专家评审重构版 v2.3
  * @date           : 2026-06-29
  *
  * @核心算法:
  *   1. 10 指手形三态编码: 每指 → '0'(<40%) / '1'(40-75%) / '2'(>75%)
  *   2. 门限微分法 (Jerk): 20 样本滑动窗口, 绝对连续差和 → 主能量轴 → 方向码
  *   3. 动态手势状态机: WAIT_STABLE(500ms) → WAIT_TRAJ → ARMED → COOLDOWN(800ms)
  *
  * @防御性设计 (v2.3 新增):
  *   ① 旋转熔断: 陀螺仪三轴角速度平方和 > 1.0 (rad/s)² 时冻结状态机,
  *      屏蔽手腕翻转引发的重力分量跨轴阶跃 → 防止误触发语音播报。
  *   ② 冷启动保护: 样本计数未达到 JERK_WINDOW(20) 前, 强制返回 DIR_NONE,
  *      防止开机时历史缓冲全 0 导致的盲判。
  *   ③ 百分比硬钳位: encode_fingers() 中对 Flex_GetPercent 返回值做
  *      [0, 100] 双向硬限幅, 防止布料形变引发的非法百分比击穿阈值判断。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "gesture.h"
#include "flex_sensor.h"
#include "jy61p.h"
#include "string.h"
#include "stdio.h"
#include "stm32f4xx_hal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  算法参数 (门限微分法)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 滑动窗口长度 (样本数)
 * @note  jerk_detect 每 10ms 调用一次, 20 个样本 = 200ms 历史窗口
 *        太短 → 方向误判; 太长 → 响应迟钝
 */
#define JERK_WINDOW     20U

/**
 * @brief 静止判定: 三轴 jerk 绝对值总和低于此门限 → 静止
 * @note  单位: ADC LSB 累加和 (非物理量, 需实测校准)
 *        当前值为经验初值, 硬件到手后须通过串口调试输出校准
 */
#define STABLE_THRESH   1500

/**
 * @brief 动态确认: 任意轴 jerk 绝对值总和高于此门限 → 置信有运动
 * @note  仅用于辅助判定, 主方向由主导轴 + 净位移符号决定
 */
#define MOTION_THRESH   3000

/**
 * @brief ★ 旋转熔断阈值: 三轴角速度平方和 (°/s)²
 * @note  JY61P 角速度单位是 °/s (非 rad/s!)
 *        60°/s → 平方 = 3600
 *        阈值 3600 (°/s)² ≈ 60°/s 的等效能级
 *        当手腕以 >60°/s 的角速度翻转时, 触发状态机冻结,
 *        因为此时重力 1g 矢量在各轴间剧烈转移,
 *        会被一阶微分误判为巨大的平移 Jerk.
 *
 *        ★ v3.0 升级: 可额外用 JY61P 的 Roll/Pitch 帧间差分
 *        做双重确认: |Roll_now - Roll_last| > 15° → 翻转
 */
#define GYRO_ROTATE_THRESH_SQ  3600.0f  /* (60°/s)², JY61P 单位 */

/**
 * @brief 手形稳定判定: 手指编码需保持不变的时长 (ms)
 * @note  500ms 足够过滤掉手势切换过程中的瞬态中间手形
 */
#define STABLE_TIME_MS  500U

/**
 * @brief 触发后冷却时间 (ms)
 * @note  防止同一手势被连续触发多次
 */
#define COOLDOWN_MS     800U

/**
 * @brief 轨迹等待超时 (ms)
 * @note  手形稳定后若在此时间内未检测到运动,
 *        则按静态手势 (方向码=0) 直接触发
 */
#define TRAJ_TIMEOUT_MS 1500U

/* ═══════════════════════════════════════════════════════════════════════════
 *  全局状态变量
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── 环形滑动窗口 (门限微分法) ── */
static int16_t ax_hist[JERK_WINDOW];
static int16_t ay_hist[JERK_WINDOW];
static int16_t az_hist[JERK_WINDOW];
static int16_t gx_hist[JERK_WINDOW];
static int16_t gy_hist[JERK_WINDOW];
static int16_t gz_hist[JERK_WINDOW];
static uint8_t  jerk_idx       = 0;    /* 当前写入位置 */
static uint8_t  jerk_warmup    = 0;    /* ★ 冷启动样本计数 (0~JERK_WINDOW) */
static uint8_t  jerk_dir       = DIR_NONE;

/* ── 旋转熔断状态 ── */
static uint8_t  rotation_frozen = 0;    /* 1=旋转熔断中, 屏蔽动态轨迹判断 */
static uint32_t rotation_frozen_since = 0; /* 熔断开始时间戳 */

/**
 * @brief 旋转熔断最短持续时间 (ms)
 * @note  翻转结束后, 如果立即恢复轨迹检测, 可能捕捉到翻转减速阶段的
 *        残余 Jerk。因此需要短暂延迟解除熔断。
 */
#define ROTATION_HOLD_MS  200U

/* ── 手势识别状态机 ── */
typedef enum {
    ST_WAIT_STABLE = 0,   /* 等待手形稳定 500ms */
    ST_WAIT_TRAJ,          /* 手形已稳定, 等待轨迹/方向确认 */
    ST_ARMED,              /* 已锁定, 执行查表播报 */
    ST_COOLDOWN            /* 冷却中 800ms, 禁止连续触发 */
} GestureState_t;

static GestureState_t gs_state       = ST_WAIT_STABLE;
static char           stable_code[12] = "00000000000";
static uint32_t       stable_start    = 0;
static uint32_t       cooldown_start  = 0;
static char           last_triggered[12] = "";

/* ── 手势词汇查表 (初始 12 词, MVP 阶段可扩展至 30~60) ── */
static const GestureEntry_t vocab[] = {
    /* ── 基础交流 ── */
    {"00000111110",  1, 0},   /* 左手平伸+右手握拳 → "你好" */
    {"00000000005",  2, 1},   /* 双手平伸+右挥 → "谢谢" */
    {"11000000000",  3, 0},   /* 左手拇指翘 → "是/好的" */
    {"11000000004",  4, 1},   /* 拇指向下 → "不是" */
    {"00000222220",  5, 0},   /* 右手全弯 → "停止" */

    /* ── 医护核心 ── */
    {"22222000004", 10, 1},   /* 左拳+下压 → "胸痛" */
    {"00000222224", 11, 1},   /* 右拳+下压 → "胸痛(右手)" */
    {"00000122220", 12, 0},   /* 右手半弯 → "头晕" */
    {"22222000000", 13, 0},   /* 左拳无移 → "喘不过气" */
    {"00100001006", 14, 1},   /* 双手食指指喉 → "窒息" */

    /* ── 紧急求助 ── */
    {"00100001003", 20, 1},   /* 双手食指+前推 → "需要帮助" */
    {"22222222221", 21, 1},   /* 双拳+上举 → "叫医生" */
};

#define VOCAB_SIZE  (sizeof(vocab) / sizeof(vocab[0]))

/* ═══════════════════════════════════════════════════════════════════════════
 *  手指编码 — 10 指三态 + 百分比硬限幅 (★ 修复 #6)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  将当前 10 指弯曲百分比编码为三态字符串
 * @param  code  [出参] 11 字节字符串 (10 指 + '\0')
 * @note   ★ v2.3 新增: Flex_GetPercent 返回值做 [0,100] 硬钳位,
 *         杜绝因布料形变/应变片挤压导致的瞬时负值或 >100 的非法百分比。
 *         编码规则:
 *           '0' = 伸直 (0~40%)
 *           '1' = 半弯 (40%~75%)
 *           '2' = 全弯 (75%~100%)
 */
static void encode_fingers(char *code)
{
    for (uint8_t h = 0; h < 2; h++) {
        for (uint8_t f = 0; f < 5; f++) {
            int16_t pct = (int16_t)Flex_GetPercent(h, f);

            /* ★ 双向硬限幅钳位 — 防御性编程 ★ */
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;

            char state;
            if (pct > (int16_t)BEND_THRESH_FULL) {
                state = '2';
            } else if (pct > (int16_t)BEND_THRESH_HALF) {
                state = '1';
            } else {
                state = '0';
            }
            code[h * 5 + f] = state;
        }
    }
    code[10] = '\0';  /* 先不填轨迹方向, 由上层状态机拼接 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  门限微分法 (Jerk) 方向检测 — 冷启动 + 旋转熔断 (★ 修复 #4, #5)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  基于 Jerk (加加速度 / 加速度的差分) 的动态方向特征检测
 * @retval DIR_NONE (0) / DIR_UP (1) / DIR_DOWN (2) / DIR_LEFT (3) /
 *         DIR_RIGHT (4) / DIR_FORWARD (5) / DIR_BACK (6)
 *
 * @算法步骤:
 *   1. 将当前 6 轴原始值 (Ax/Ay/Az + Gx/Gy/Gz) 填入环形滑动窗口
 *      ★ v3.0: 数据源从 MPU6050 改为 JY61P (acc_raw[] + gyro_raw[])
 *   2. ★ 冷启动保护: 若样本数 < JERK_WINDOW 则直接返回 DIR_NONE
 *   3. ★ 旋转熔断: 读取 JY61P_Right 当前角速度 (°/s), 计算平方和,
 *      若 > GYRO_ROTATE_THRESH_SQ (3600 = 60°/s)² 则冻结并返回 DIR_NONE
 *   4. 计算窗口内三轴绝对连续差和 (jerk_x/jerk_y/jerk_z)
 *   5. 若总 jerk < STABLE_THRESH → 静止, 返回 DIR_NONE
 *   6. 识别主导轴 (jerk 最大值所在轴) → 计算该轴净位移方向
 *   7. 返回对应方向码
 *
 * @note   ★ v3.0 JY61P 适配:
 *        - acc_raw 量程 ±16g (2048 LSB/g), 约 MPU6050 的 1/8
 *        - gyro_raw 量程 ±2000°/s (16.4 LSB/(°/s))
 *        - STABLE_THRESH 需硬件实测后重新校准 (当前值 1500 为 MPU6050 旧值)
 *        - 方向判定基于净位移 (最后一个样本 - 第一个样本) 的符号
 */
static uint8_t jerk_detect(void)
{
    /* ── 1. 填入当前样本 (JY61P 右手原始值) ── */
    JY61P_Data_t *r = &JY61P_Right;

    ax_hist[jerk_idx] = r->acc_raw[0];
    ay_hist[jerk_idx] = r->acc_raw[1];
    az_hist[jerk_idx] = r->acc_raw[2];
    gx_hist[jerk_idx] = r->gyro_raw[0];
    gy_hist[jerk_idx] = r->gyro_raw[1];
    gz_hist[jerk_idx] = r->gyro_raw[2];
    jerk_idx = (jerk_idx + 1U) % JERK_WINDOW;

    /* ── 2. ★ 冷启动保护: 窗口未满 20 样本则静默 ── */
    if (jerk_warmup < JERK_WINDOW) {
        jerk_warmup++;
        return DIR_NONE;
    }

    /* ── 3. ★ 旋转熔断: 检测手腕翻转 ── */
    /* 用右手 JY61P 物理角速度 (°/s) 计算旋转能量 */
    float gyro_energy = r->gyro[0] * r->gyro[0]
                      + r->gyro[1] * r->gyro[1]
                      + r->gyro[2] * r->gyro[2];
    if (gyro_energy > GYRO_ROTATE_THRESH_SQ) {
        /* 手腕正在快速翻转 (>60°/s 等效) — 冻结 */
        if (!rotation_frozen) {
            rotation_frozen = 1;
            rotation_frozen_since = HAL_GetTick();
        }
        return DIR_NONE;
    }

    /* 旋转结束后维持 ROTATION_HOLD_MS 的延迟解除,
     * 防止翻转减速阶段的残余 Jerk 继续污染特征提取 */
    if (rotation_frozen) {
        uint32_t now = HAL_GetTick();
        if (now - rotation_frozen_since > ROTATION_HOLD_MS) {
            rotation_frozen = 0;
        } else {
            return DIR_NONE;  /* 熔断保持中 */
        }
    }

    /* ── 4. 计算三轴 jerk 绝对差和 ── */
    int32_t jerk_x = 0, jerk_y = 0, jerk_z = 0;
    int32_t jerk_gyro = 0;
    for (uint8_t i = 1; i < JERK_WINDOW; i++) {
        uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
        uint8_t prev = (jerk_idx + i - 1U) % JERK_WINDOW;

        int32_t dx = (int32_t)ax_hist[curr] - (int32_t)ax_hist[prev];
        int32_t dy = (int32_t)ay_hist[curr] - (int32_t)ay_hist[prev];
        int32_t dz = (int32_t)az_hist[curr] - (int32_t)az_hist[prev];
        jerk_x += (dx >= 0) ? dx : -dx;
        jerk_y += (dy >= 0) ? dy : -dy;
        jerk_z += (dz >= 0) ? dz : -dz;

        int32_t dgx = (int32_t)gx_hist[curr] - (int32_t)gx_hist[prev];
        int32_t dgy = (int32_t)gy_hist[curr] - (int32_t)gy_hist[prev];
        int32_t dgz = (int32_t)gz_hist[curr] - (int32_t)gz_hist[prev];
        jerk_gyro += (dgx >= 0) ? dgx : -dgx;
        jerk_gyro += (dgy >= 0) ? dgy : -dgy;
        jerk_gyro += (dgz >= 0) ? dgz : -dgz;
    }

    int32_t jerk_total = jerk_x + jerk_y + jerk_z;

    /* ── 5. 静止判定 ── */
    if (jerk_total < STABLE_THRESH) return DIR_NONE;

    /* ── 6. 识别主导轴 → 计算净位移方向 ── */
    if (jerk_x > jerk_y && jerk_x > jerk_z) {
        /* X 轴主导 → 计算窗口内首尾净位移的符号 */
        int32_t net = 0;
        for (uint8_t i = 1; i < JERK_WINDOW; i++) {
            uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
            uint8_t prev = (jerk_idx + i - 1U) % JERK_WINDOW;
            net += (int32_t)ax_hist[curr] - (int32_t)ax_hist[prev];
        }
        return (net > 0) ? DIR_RIGHT : DIR_LEFT;
    }
    if (jerk_y > jerk_x && jerk_y > jerk_z) {
        int32_t net = 0;
        for (uint8_t i = 1; i < JERK_WINDOW; i++) {
            uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
            uint8_t prev = (jerk_idx + i - 1U) % JERK_WINDOW;
            net += (int32_t)ay_hist[curr] - (int32_t)ay_hist[prev];
        }
        return (net > 0) ? DIR_UP : DIR_DOWN;
    }
    if (jerk_z > jerk_x && jerk_z > jerk_y) {
        int32_t net = 0;
        for (uint8_t i = 1; i < JERK_WINDOW; i++) {
            uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
            uint8_t prev = (jerk_idx + i - 1U) % JERK_WINDOW;
            net += (int32_t)az_hist[curr] - (int32_t)az_hist[prev];
        }
        return (net > 0) ? DIR_BACK : DIR_FORWARD;
    }

    /* 旋转为主 (jerk_gyro 主导) → 不判方向, 归静止 */
    return DIR_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  手势引擎初始化
 * @note   清空所有历史缓冲、状态机复位。开机或系统复位后调用一次。
 */
void Gesture_Init(void)
{
    memset(ax_hist,  0, sizeof(ax_hist));
    memset(ay_hist,  0, sizeof(ay_hist));
    memset(az_hist,  0, sizeof(az_hist));
    memset(gx_hist,  0, sizeof(gx_hist));
    memset(gy_hist,  0, sizeof(gy_hist));
    memset(gz_hist,  0, sizeof(gz_hist));
    memset(stable_code, 0, sizeof(stable_code));
    memset(last_triggered, 0, sizeof(last_triggered));

    jerk_idx        = 0;
    jerk_warmup     = 0;   /* ★ 冷启动计数器归零 */
    jerk_dir        = DIR_NONE;
    rotation_frozen = 0;
    gs_state        = ST_WAIT_STABLE;
    stable_start    = 0;
}

/**
 * @brief  核心手势判定 (每 50ms 调用)
 * @return GestureResult_t:
 *         .active     = 1 表示触发了一个有效手势
 *         .file_index = 对应 DFPlayer MP3 文件编号
 *         .code_str   = 手势编码字符串 (调试用)
 *
 * @状态机:
 *   ST_WAIT_STABLE → 手形不变 500ms → ST_WAIT_TRAJ
 *   ST_WAIT_TRAJ   → 检测到方向 / 超时 1500ms → ST_ARMED
 *   ST_ARMED       → 查表 + 触发语音/振动 → ST_COOLDOWN
 *   ST_COOLDOWN    → 800ms 冷却 → ST_WAIT_STABLE
 */
GestureResult_t Gesture_Evaluate(void)
{
    GestureResult_t res = {0, 0, ""};
    uint32_t now = HAL_GetTick();

    /* ── 生成当前 10 指三态编码 ── */
    char cur_code[11];
    encode_fingers(cur_code);
    cur_code[10] = '\0';

    /* ── 门限微分法检测轨迹方向 ── */
    uint8_t dir = jerk_detect();

    /* ── 手势识别状态机 ── */
    switch (gs_state) {

    case ST_WAIT_STABLE:
        /* 手形编码变化 → 重置稳定计时 */
        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
        } else if ((now - stable_start) > STABLE_TIME_MS) {
            /* 手形已保持 500ms → 进入轨迹等待 */
            gs_state = ST_WAIT_TRAJ;
        }
        break;

    case ST_WAIT_TRAJ:
        /* 检测到方向动作 → 锁定并进入 ARMED */
        if (dir != DIR_NONE) {
            jerk_dir = dir;
            gs_state = ST_ARMED;
        } else if ((now - stable_start) > TRAJ_TIMEOUT_MS) {
            /* 超时无动作 → 按静态手势处理 (方向码=0) */
            jerk_dir = DIR_NONE;
            gs_state = ST_ARMED;
        }
        /* 手形在等待期间变化 → 回退到 ST_WAIT_STABLE */
        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
            gs_state = ST_WAIT_STABLE;
        }
        break;

    case ST_ARMED: {
        /* ── 组装 11 位完整编码: 10 指 + 1 方向 ── */
        char full_code[12];
        snprintf(full_code, sizeof(full_code), "%s%c",
                 stable_code,
                 (jerk_dir == DIR_NONE) ? '0' : (char)('0' + jerk_dir));

        /* 防重入: 同一编码不连续触发两次 */
        if (strcmp(full_code, last_triggered) != 0) {
            /* 查表匹配 */
            uint8_t found = 0;
            for (uint16_t i = 0; i < VOCAB_SIZE; i++) {
                if (strcmp(full_code, vocab[i].code) == 0) {
                    res.active     = 1;
                    res.file_index = vocab[i].mp3_file;
                    strncpy(res.code_str, full_code, 11);
                    res.code_str[11] = '\0';
                    strcpy(last_triggered, full_code);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* 未匹配任何已知手势 → 记录但不触发 */
                strcpy(last_triggered, full_code);
            }
        }
        cooldown_start = now;
        gs_state = ST_COOLDOWN;
        break;
    }

    case ST_COOLDOWN:
        if ((now - cooldown_start) > COOLDOWN_MS) {
            /* 冷却结束 → 清空手形记录, 重新等待 */
            gs_state = ST_WAIT_STABLE;
            memset(stable_code, 0, sizeof(stable_code));
            stable_start = now;
        }
        break;
    }

    return res;
}

/**
 * @brief  获取当前 10 指编码 (不含方向位)
 * @param  code_out  [出参] 至少 11 字节的缓冲区
 * @note   用于教学模式: 学生做出手势 → 获取编码 → 与目标对比
 */
void Gesture_GetCurrentCode(char *code_out)
{
    encode_fingers(code_out);
}

/**
 * @brief  逐指对比当前手势与目标模板
 * @param  target_code  目标 10 字编码 (如 "1100000000")
 * @return 错误位掩码: bit0=左手拇指, bit1=左手食指, ...
 *         0 表示完全匹配
 */
uint8_t Gesture_Compare(const char *target_code)
{
    uint8_t errors = 0;
    char cur[11];
    encode_fingers(cur);
    for (uint8_t i = 0; i < 10; i++) {
        if (cur[i] != target_code[i]) {
            errors |= (uint8_t)(1U << i);
        }
    }
    return errors;
}

/**
 * @brief  标定: 将当前 ADC 值记录为伸直 (MIN) 或弯曲 (MAX) 参考
 * @param  hand      0=右手, 1=左手
 * @param  cal_type  0=MIN (伸直), 1=MAX (弯曲)
 * @note   由蓝牙指令 <CAL:MIN> / <CAL:MAX> 触发
 */
void Gesture_Calibrate(uint8_t hand, uint8_t cal_type)
{
    Flex_Finger_t *h = hand ? Hand_Left : Hand_Right;

    for (uint8_t f = 0; f < 5; f++) {
        uint16_t raw = h[f].current_raw;

        /* ★ 防御: 标定值必须在合理 ADC 范围内 (0~4095) ★ */
        if (raw > 4095U) raw = 4095U;

        if (cal_type == 0U) {
            h[f].adc_min = raw;
        } else {
            h[f].adc_max = raw;
        }
    }
}
