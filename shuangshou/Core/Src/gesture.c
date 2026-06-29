/**
  ******************************************************************************
  * @file           : gesture.c
  * @brief          : 双手手势识别引擎 — 手形编码 + 门限微分法 + 防御性保护
  * @author         : 车架师重构版 v2.4
  * @date           : 2026-06-29
  *
  * @防御性升级 (v2.3 → v2.4):
  *   ★ ④ 双手异步松弛锁存窗 (Latching Window):
  *       新增 ST_LATCH 状态 + COOLDOWN_WINDOW_MS (250ms) 松弛存活期。
  *       当单手先捕获到轨迹特征时, 不立即触发, 而是打开 250ms 时间窗口,
  *       等待另一只手的手指形态跟上。窗口内持续更新手形编码,
  *       窗口结束后才触发最终查表匹配。
  *       解决人类生理学上双手动作异步到达 (50~200ms 延迟) 的漏判 Bug。
  *
  *   ★ ③ 旋转熔断: 陀螺仪角速度平方和 >3600 (°/s)² 冻结 (v3.0 JY61P 适配)
  *   ★ ② 冷启动保护: 20 样本静默期 (保留)
  *   ★ ① 百分比硬钳位: [0,100] (保留)
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
 *  算法参数
 * ═══════════════════════════════════════════════════════════════════════════ */

#define JERK_WINDOW            20U     /* 200ms 滑动窗口 @ 10ms */
#define STABLE_THRESH          1500    /* 静止 Jerk 门限 (待 JY61P 校准) */
#define GYRO_ROTATE_THRESH_SQ  3600.0f /* (60°/s)², JY61P 单位 °/s */

/**
 * @brief 手形稳定判定: 手指编码保持不变的时长 (ms)
 */
#define STABLE_TIME_MS         500U

/**
 * @brief 轨迹等待超时 (ms): 手形稳定后等方向
 */
#define TRAJ_TIMEOUT_MS        1500U

/**
 * @brief ★ 双手异步松弛锁存窗 (ms) — v2.4 新增
 * @note  人类双手动作的时间差通常在 50~200ms 之间。
 *        当单手先捕获到轨迹特征进入 ARMED 后,
 *        不立即触发, 而是打开 250ms 的时间窗口,
 *        等待双手编码最终同步。
 *        窗口内每一帧(50ms)都更新手形编码,
 *        窗口关闭后以最新编码查表触发。
 */
#define COOLDOWN_WINDOW_MS     250U

/**
 * @brief 触发后冷却时间 (ms): 防止连续重复播报
 */
#define COOLDOWN_MS            800U

/* ═══════════════════════════════════════════════════════════════════════════
 *  全局状态变量
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Jerk 滑动窗口 ── */
static int16_t ax_hist[JERK_WINDOW], ay_hist[JERK_WINDOW], az_hist[JERK_WINDOW];
static int16_t gx_hist[JERK_WINDOW], gy_hist[JERK_WINDOW], gz_hist[JERK_WINDOW];
static uint8_t  jerk_idx       = 0;
static uint8_t  jerk_warmup    = 0;
static uint8_t  jerk_dir       = DIR_NONE;

/* ── 旋转熔断 ── */
static uint8_t  rotation_frozen = 0;
static uint32_t rotation_frozen_since = 0;
#define ROTATION_HOLD_MS       200U

/* ── ★ 手势识别状态机 (v2.4 新增 ST_LATCH) ── */
typedef enum {
    ST_WAIT_STABLE = 0,   /* 等待手形稳定 500ms           */
    ST_WAIT_TRAJ,          /* 手形已稳定, 等待轨迹方向      */
    ST_ARMED,              /* 已锁定, 准备触发              */
    ST_LATCH,              /* ★ 松弛锁存窗 (250ms)         */
    ST_COOLDOWN            /* 触发后冷却 800ms              */
} GestureState_t;

static GestureState_t gs_state       = ST_WAIT_STABLE;
static char           stable_code[12] = "00000000000";
static uint32_t       stable_start    = 0;
static uint32_t       cooldown_start  = 0;
static uint32_t       latch_start     = 0;  /* ★ 锁存窗启动时刻 */
static char           latch_best_code[12];  /* ★ 窗口内最佳匹配编码 */
static uint8_t        latch_best_file;      /* ★ 窗口内最佳 MP3 编号 */
static uint8_t        latch_has_match;      /* ★ 窗口内是否有命中 */
static char           last_triggered[12] = "";

/* ── 词汇表 ── */
static const GestureEntry_t vocab[] = {
    {"00000111110",  1, 0},   /* "你好" */
    {"00000000005",  2, 1},   /* "谢谢" */
    {"11000000000",  3, 0},   /* "是/好的" */
    {"11000000004",  4, 1},   /* "不是" */
    {"00000222220",  5, 0},   /* "停止" */
    {"22222000004", 10, 1},   /* "胸痛" */
    {"00000222224", 11, 1},   /* "胸痛(右手)" */
    {"00000122220", 12, 0},   /* "头晕" */
    {"22222000000", 13, 0},   /* "喘不过气" */
    {"00100001006", 14, 1},   /* "窒息" */
    {"00100001003", 20, 1},   /* "需要帮助" */
    {"22222222221", 21, 1},   /* "叫医生" */
};
#define VOCAB_SIZE  (sizeof(vocab) / sizeof(vocab[0]))

/* ═══════════════════════════════════════════════════════════════════════════
 *  手指编码 (保留)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void encode_fingers(char *code)
{
    for (uint8_t h = 0; h < 2; h++) {
        for (uint8_t f = 0; f < 5; f++) {
            int16_t pct = (int16_t)Flex_GetPercent(h, f);
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
            char state;
            if (pct > (int16_t)BEND_THRESH_FULL)      state = '2';
            else if (pct > (int16_t)BEND_THRESH_HALF) state = '1';
            else                                      state = '0';
            code[h * 5 + f] = state;
        }
    }
    code[10] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  门限微分法 (Jerk) — 冷启动 + 旋转熔断 (保留)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t jerk_detect(void)
{
    JY61P_Data_t *r = &JY61P_Right;

    ax_hist[jerk_idx] = r->acc_raw[0];
    ay_hist[jerk_idx] = r->acc_raw[1];
    az_hist[jerk_idx] = r->acc_raw[2];
    gx_hist[jerk_idx] = r->gyro_raw[0];
    gy_hist[jerk_idx] = r->gyro_raw[1];
    gz_hist[jerk_idx] = r->gyro_raw[2];
    jerk_idx = (jerk_idx + 1U) % JERK_WINDOW;

    if (jerk_warmup < JERK_WINDOW) { jerk_warmup++; return DIR_NONE; }

    float gyro_energy = r->gyro[0] * r->gyro[0]
                      + r->gyro[1] * r->gyro[1]
                      + r->gyro[2] * r->gyro[2];
    if (gyro_energy > GYRO_ROTATE_THRESH_SQ) {
        if (!rotation_frozen) {
            rotation_frozen = 1;
            rotation_frozen_since = HAL_GetTick();
        }
        return DIR_NONE;
    }
    if (rotation_frozen) {
        uint32_t now = HAL_GetTick();
        if (now - rotation_frozen_since > ROTATION_HOLD_MS)
            rotation_frozen = 0;
        else
            return DIR_NONE;
    }

    int32_t jerk_x = 0, jerk_y = 0, jerk_z = 0;
    for (uint8_t i = 1; i < JERK_WINDOW; i++) {
        uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
        uint8_t prev = (jerk_idx + i - 1U) % JERK_WINDOW;
        int32_t dx = (int32_t)ax_hist[curr] - (int32_t)ax_hist[prev];
        int32_t dy = (int32_t)ay_hist[curr] - (int32_t)ay_hist[prev];
        int32_t dz = (int32_t)az_hist[curr] - (int32_t)az_hist[prev];
        jerk_x += (dx >= 0) ? dx : -dx;
        jerk_y += (dy >= 0) ? dy : -dy;
        jerk_z += (dz >= 0) ? dz : -dz;
    }
    int32_t jerk_total = jerk_x + jerk_y + jerk_z;
    if (jerk_total < STABLE_THRESH) return DIR_NONE;

    if (jerk_x > jerk_y && jerk_x > jerk_z) {
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
    return DIR_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  词汇查表辅助
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t lookup_vocab(const char *full_code, uint16_t *file_out)
{
    for (uint16_t i = 0; i < VOCAB_SIZE; i++) {
        if (strcmp(full_code, vocab[i].code) == 0) {
            *file_out = vocab[i].mp3_file;
            return 1;  /* 命中 */
        }
    }
    return 0;  /* 未命中 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════════════════════════════════════ */

void Gesture_Init(void)
{
    memset(ax_hist, 0, sizeof(ax_hist)); memset(ay_hist, 0, sizeof(ay_hist));
    memset(az_hist, 0, sizeof(az_hist)); memset(gx_hist, 0, sizeof(gx_hist));
    memset(gy_hist, 0, sizeof(gy_hist)); memset(gz_hist, 0, sizeof(gz_hist));
    memset(stable_code, 0, sizeof(stable_code));
    memset(last_triggered, 0, sizeof(last_triggered));
    memset(latch_best_code, 0, sizeof(latch_best_code));

    jerk_idx        = 0;
    jerk_warmup     = 0;
    jerk_dir        = DIR_NONE;
    rotation_frozen = 0;
    gs_state        = ST_WAIT_STABLE;
    stable_start    = 0;
    latch_start     = 0;
    latch_best_file = 0;
    latch_has_match = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ★★★ 核心判定 — v2.4 五态状态机 (引入 ST_LATCH 松弛锁存窗)
 *
 *  状态迁移图:
 *   ST_WAIT_STABLE ──(手形稳定500ms)──→ ST_WAIT_TRAJ
 *   ST_WAIT_TRAJ ────(检测到方向/超时)─→ ST_ARMED
 *   ST_ARMED ────────(组装编码)────────→ ST_LATCH    ★ 新增
 *   ST_LATCH ───────(250ms 窗口关闭)───→ ST_COOLDOWN  ★ 新增
 *   ST_COOLDOWN ────(800ms)───────────→ ST_WAIT_STABLE
 *
 *  核心思想 (双手异步松弛):
 *   人类双手运动存在天然异步性 (50~200ms 延迟)。
 *   当一手先完成动作进入 ARMED 时, 另一手可能还在过渡。
 *   ST_LATCH 提供一个 250ms 的 "等待窗口",
 *   在此期间持续更新手形编码, 窗口关闭后才查表触发。
 *   → 确保双手编码在最佳同步点被捕获, 消灭异步漏判。
 * ═══════════════════════════════════════════════════════════════════════════ */

GestureResult_t Gesture_Evaluate(void)
{
    GestureResult_t res = {0, 0, ""};
    uint32_t now = HAL_GetTick();
    uint8_t dir;

    /* ── 生成当前 10 指编码 ── */
    char cur_code[11];
    encode_fingers(cur_code);
    cur_code[10] = '\0';

    /* ── 门限微分法方向 ── */
    dir = jerk_detect();

    /* ── 五态状态机 ── */
    switch (gs_state) {

    /* ═══════════════════════════════════════════════════
     *  ST_WAIT_STABLE: 等待手形保持稳定 500ms
     * ═══════════════════════════════════════════════════ */
    case ST_WAIT_STABLE:
        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
        } else if ((now - stable_start) > STABLE_TIME_MS) {
            gs_state = ST_WAIT_TRAJ;
        }
        break;

    /* ═══════════════════════════════════════════════════
     *  ST_WAIT_TRAJ: 等待轨迹方向 (最多 1500ms)
     * ═══════════════════════════════════════════════════ */
    case ST_WAIT_TRAJ:
        if (dir != DIR_NONE) {
            jerk_dir = dir;
            gs_state = ST_ARMED;
        } else if ((now - stable_start) > TRAJ_TIMEOUT_MS) {
            jerk_dir = DIR_NONE;
            gs_state = ST_ARMED;
        }
        /* 手形在等待期间变动 → 回退 */
        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
            gs_state = ST_WAIT_STABLE;
        }
        break;

    /* ═══════════════════════════════════════════════════
     *  ST_ARMED: 锁定手形 + 方向 → 进入松弛锁存窗
     * ═══════════════════════════════════════════════════ */
    case ST_ARMED: {
        /* 组装 11 位编码 */
        char full_code[12];
        snprintf(full_code, sizeof(full_code), "%s%c",
                 stable_code,
                 (jerk_dir == DIR_NONE) ? '0' : (char)('0' + jerk_dir));

        if (strcmp(full_code, last_triggered) != 0) {
            uint16_t file_tmp = 0;
            if (lookup_vocab(full_code, &file_tmp)) {
                /* 立即命中 → 记录最佳, 但不触发 */
                strcpy(latch_best_code, full_code);
                latch_best_file = (uint8_t)file_tmp;
                latch_has_match = 1;
            } else {
                /* 未命中 → 也记录编码, 窗口内可能被后续编码命中 */
                strcpy(latch_best_code, full_code);
                latch_has_match = 0;
            }
        }
        latch_start = now;
        gs_state = ST_LATCH;
        break;
    }

    /* ═══════════════════════════════════════════════════
     *  ★ ST_LATCH: 松弛锁存窗 (250ms)
     *
     *  窗口内每一帧 (50ms) 都:
     *    ① 更新手形编码 (双手可能还在异步到达途中)
     *    ② 查表 → 若命中, 更新最佳结果
     *    ③ 检查锁存窗是否到期
     *
     *  窗口关闭时:
     *    - 若有命中 → 触发语音/振动
     *    - 若无命中 → 静默进入冷却
     * ═══════════════════════════════════════════════════ */
    case ST_LATCH: {
        /* ── 组装当前时刻的编码 ── */
        char latch_code[12];
        snprintf(latch_code, sizeof(latch_code), "%s%c",
                 cur_code,
                 (dir == DIR_NONE) ? '0' : (char)('0' + dir));

        /* ── 查表更新 ── */
        uint16_t file_tmp = 0;
        if (lookup_vocab(latch_code, &file_tmp)) {
            /* 命中! 更新锁存窗内的最佳结果 */
            if (strcmp(latch_code, latch_best_code) != 0) {
                strcpy(latch_best_code, latch_code);
                latch_best_file = (uint8_t)file_tmp;
                latch_has_match = 1;
            }
        }

        /* ── 检查锁存窗是否到期 ── */
        if ((now - latch_start) > COOLDOWN_WINDOW_MS) {
            /* 窗口关闭 — 输出最佳结果 */
            if (latch_has_match) {
                res.active     = 1;
                res.file_index = latch_best_file;
                strncpy(res.code_str, latch_best_code, 11);
                res.code_str[11] = '\0';
                strcpy(last_triggered, latch_best_code);
            } else {
                /* 窗口内无命中 → 记录编码防重入, 但不触发 */
                strcpy(last_triggered, latch_best_code);
            }
            cooldown_start = now;
            gs_state = ST_COOLDOWN;
        }
        break;
    }

    /* ═══════════════════════════════════════════════════
     *  ST_COOLDOWN: 800ms 冷却
     * ═══════════════════════════════════════════════════ */
    case ST_COOLDOWN:
        if ((now - cooldown_start) > COOLDOWN_MS) {
            gs_state = ST_WAIT_STABLE;
            memset(stable_code, 0, sizeof(stable_code));
            stable_start = now;
            latch_has_match = 0;
            latch_best_file = 0;
        }
        break;
    }

    return res;
}

/* ── 其余 API 不变 ── */

void Gesture_GetCurrentCode(char *code_out) { encode_fingers(code_out); }

uint8_t Gesture_Compare(const char *target_code)
{
    uint8_t errors = 0;
    char cur[11];
    encode_fingers(cur);
    for (uint8_t i = 0; i < 10; i++)
        if (cur[i] != target_code[i]) errors |= (uint8_t)(1U << i);
    return errors;
}

void Gesture_Calibrate(uint8_t hand, uint8_t cal_type)
{
    Flex_Finger_t *h = hand ? Hand_Left : Hand_Right;
    for (uint8_t f = 0; f < 5; f++) {
        uint16_t raw = h[f].current_raw;
        if (raw > 4095U) raw = 4095U;
        if (cal_type == 0U) h[f].adc_min = raw;
        else                h[f].adc_max = raw;
    }
}
