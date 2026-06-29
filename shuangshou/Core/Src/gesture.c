#include "gesture.h"
#include "flex_sensor.h"
#include "mpu6050.h"
#include "string.h"
#include "stdio.h"
#include "stm32f4xx_hal.h"

/* ── 门限微分法参数 ── */
#define JERK_WINDOW    20       /* 200ms @ 10ms 周期 */
#define JERK_THRESHOLD 8000     /* 加加速度和门限，需实测校准 */
#define STABLE_THRESH  1500     /* 静止判定门限 */

/* ── 稳定判定 ── */
#define STABLE_TIME_MS 500      /* 手形需保持 500ms */
#define COOLDOWN_MS    800      /* 触发后冷却 800ms */

/* ── 轨迹方向检测 — 环形缓冲 ── */
static int16_t ax_hist[JERK_WINDOW];
static int16_t ay_hist[JERK_WINDOW];
static int16_t az_hist[JERK_WINDOW];
static int16_t gx_hist[JERK_WINDOW];
static int16_t gy_hist[JERK_WINDOW];
static int16_t gz_hist[JERK_WINDOW];
static uint8_t  jerk_idx = 0;
static uint8_t  jerk_dir = DIR_NONE;

/* ── 手形稳定状态机 ── */
typedef enum {
    ST_WAIT_STABLE = 0,
    ST_WAIT_TRAJ,
    ST_ARMED,
    ST_COOLDOWN
} GestureState_t;

static GestureState_t gs_state = ST_WAIT_STABLE;
static char    stable_code[12] = "00000000000";
static uint32_t stable_start = 0;
static uint32_t cooldown_start = 0;
static char    last_triggered[12] = "";

/* ── 查表（初始词库，可动态扩展） ── */
static const GestureEntry_t vocab[] = {
    /* 基础交流 */
    {"00000111110", 1, 0},   /* 左手平伸+右手握拳 → 你好 */
    {"00000000005", 2, 1},   /* 双手平伸+右挥 → 谢谢 */
    {"11000000000", 3, 0},   /* 左手拇指翘 → 是/好的 */
    {"11000000004", 4, 1},   /* 拇指向下 → 不是 */
    {"00000222220", 5, 0},   /* 右手全弯 → 停止 */

    /* 医护核心 */
    {"22222000004", 10, 1},  /* 左拳+下压 → 胸痛 */
    {"00000222224", 11, 1},  /* 右拳+下压 → 胸痛(右手) */
    {"00000122220", 12, 0},  /* 右手半弯 → 头晕 */
    {"22222000000", 13, 0},  /* 左拳无移 → 喘不过气 */
    {"00100001006", 14, 1},  /* 双手食指指喉 → 窒息 */

    /* 紧急 */
    {"00100001003", 20, 1},  /* 双手食指+前推 → 需要帮助 */
    {"22222222221", 21, 1},  /* 双拳+上举 → 叫医生 */
};

#define VOCAB_SIZE (sizeof(vocab) / sizeof(vocab[0]))

/* ── 生成 10 位手形编码 ── */
static void encode_fingers(char *code)
{
    for (uint8_t h = 0; h < 2; h++) {
        for (uint8_t f = 0; f < 5; f++) {
            uint8_t pct = Flex_GetPercent(h, f);
            char state;
            if (pct > BEND_THRESH_FULL)      state = '2';
            else if (pct > BEND_THRESH_HALF) state = '1';
            else                             state = '0';
            code[h * 5 + f] = state;
        }
    }
    code[10] = '\0';  /* 先不填轨迹 */
}

/* ── 门限微分法：维护滑动窗口 + 判定方向 ── */
static uint8_t jerk_detect(void)
{
    /* 取原始加速度（用右手为主，左手辅助） */
    MPU6050_t *r = &MPU6050_Right;
    ax_hist[jerk_idx] = r->Accel_X_RAW;
    ay_hist[jerk_idx] = r->Accel_Y_RAW;
    az_hist[jerk_idx] = r->Accel_Z_RAW;
    gx_hist[jerk_idx] = r->Gyro_X_RAW;
    gy_hist[jerk_idx] = r->Gyro_Y_RAW;
    gz_hist[jerk_idx] = r->Gyro_Z_RAW;
    jerk_idx = (jerk_idx + 1) % JERK_WINDOW;

    /* 计算三轴 jerk 绝对值和 */
    int32_t jerk_x = 0, jerk_y = 0, jerk_z = 0;
    int32_t jerk_gyro = 0;
    for (uint8_t i = 1; i < JERK_WINDOW; i++) {
        uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
        uint8_t prev = (jerk_idx + i - 1) % JERK_WINDOW;

        int32_t dx = ax_hist[curr] - ax_hist[prev];
        int32_t dy = ay_hist[curr] - ay_hist[prev];
        int32_t dz = az_hist[curr] - az_hist[prev];
        jerk_x += (dx >= 0) ? dx : -dx;
        jerk_y += (dy >= 0) ? dy : -dy;
        jerk_z += (dz >= 0) ? dz : -dz;

        int32_t dgx = gx_hist[curr] - gx_hist[prev];
        int32_t dgy = gy_hist[curr] - gy_hist[prev];
        int32_t dgz = gz_hist[curr] - gz_hist[prev];
        jerk_gyro += (dgx >= 0) ? dgx : -dgx;
        jerk_gyro += (dgy >= 0) ? dgy : -dgy;
        jerk_gyro += (dgz >= 0) ? dgz : -dgz;
    }

    int32_t jerk_total = jerk_x + jerk_y + jerk_z;

    /* 静止 */
    if (jerk_total < STABLE_THRESH) return DIR_NONE;

    /* 运动检测：哪个轴 jerk 最大 */
    if (jerk_x > jerk_y && jerk_x > jerk_z) {
        int32_t net = 0;
        for (uint8_t i = 1; i < JERK_WINDOW; i++) {
            uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
            uint8_t prev = (jerk_idx + i - 1) % JERK_WINDOW;
            net += ax_hist[curr] - ax_hist[prev];
        }
        return (net > 0) ? DIR_RIGHT : DIR_LEFT;
    }
    if (jerk_y > jerk_x && jerk_y > jerk_z) {
        int32_t net = 0;
        for (uint8_t i = 1; i < JERK_WINDOW; i++) {
            uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
            uint8_t prev = (jerk_idx + i - 1) % JERK_WINDOW;
            net += ay_hist[curr] - ay_hist[prev];
        }
        return (net > 0) ? DIR_UP : DIR_DOWN;
    }
    if (jerk_z > jerk_x && jerk_z > jerk_y) {
        int32_t net = 0;
        for (uint8_t i = 1; i < JERK_WINDOW; i++) {
            uint8_t curr = (jerk_idx + i) % JERK_WINDOW;
            uint8_t prev = (jerk_idx + i - 1) % JERK_WINDOW;
            net += az_hist[curr] - az_hist[prev];
        }
        return (net > 0) ? DIR_BACK : DIR_FORWARD;
    }
    /* 旋转为主 → 暂归为静止 */
    return DIR_NONE;
}

/* ── 初始化 ── */
void Gesture_Init(void)
{
    memset(ax_hist, 0, sizeof(ax_hist));
    memset(ay_hist, 0, sizeof(ay_hist));
    memset(az_hist, 0, sizeof(az_hist));
    memset(stable_code, 0, sizeof(stable_code));
    memset(last_triggered, 0, sizeof(last_triggered));
    jerk_idx = 0;
    jerk_dir = DIR_NONE;
    gs_state = ST_WAIT_STABLE;
    stable_start = 0;
}

/* ── 核心判定 ── */
GestureResult_t Gesture_Evaluate(void)
{
    GestureResult_t res = {0, 0, ""};
    uint32_t now = HAL_GetTick();

    /* 生成当前 10 指编码 */
    char cur_code[11];
    encode_fingers(cur_code);
    cur_code[10] = '\0';

    /* 门限微分法检测轨迹方向 */
    uint8_t dir = jerk_detect();

    /* ── 状态机 ── */
    switch (gs_state) {

    case ST_WAIT_STABLE:
        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
        } else if (now - stable_start > STABLE_TIME_MS) {
            gs_state = ST_WAIT_TRAJ;
        }
        break;

    case ST_WAIT_TRAJ:
        if (dir != DIR_NONE) {
            jerk_dir = dir;
            gs_state = ST_ARMED;
        } else if (now - stable_start > 1500) {
            /* 超时，按静态手势处理 */
            jerk_dir = DIR_NONE;
            gs_state = ST_ARMED;
        }
        /* 手形变了？回到 WAIT_STABLE */
        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
            gs_state = ST_WAIT_STABLE;
        }
        break;

    case ST_ARMED: {
        /* 组装完整 11 位编码 */
        char full_code[12];
        snprintf(full_code, 12, "%s%c",
                 stable_code,
                 (jerk_dir == DIR_NONE) ? '0' : ('0' + jerk_dir));

        if (strcmp(full_code, last_triggered) != 0) {
            /* 查表 */
            for (uint16_t i = 0; i < VOCAB_SIZE; i++) {
                if (strcmp(full_code, vocab[i].code) == 0) {
                    res.active = 1;
                    res.file_index = vocab[i].mp3_file;
                    strncpy(res.code_str, full_code, 11);
                    strcpy(last_triggered, full_code);
                    cooldown_start = now;
                    gs_state = ST_COOLDOWN;
                    return res;
                }
            }
            /* 未匹配 → 冷却但无触发 */
            strcpy(last_triggered, full_code);
            cooldown_start = now;
            gs_state = ST_COOLDOWN;
        }
        break;
    }

    case ST_COOLDOWN:
        if (now - cooldown_start > COOLDOWN_MS) {
            gs_state = ST_WAIT_STABLE;
            memset(stable_code, 0, sizeof(stable_code));
            stable_start = now;
        }
        break;
    }

    return res;
}

/* ── 获取当前编码 ── */
void Gesture_GetCurrentCode(char *code_out)
{
    encode_fingers(code_out);
}

/* ── 逐指对比 ── */
uint8_t Gesture_Compare(const char *target_code)
{
    uint8_t errors = 0;
    char cur[11];
    encode_fingers(cur);
    for (uint8_t i = 0; i < 10; i++) {
        if (cur[i] != target_code[i]) {
            errors |= (1 << i);
        }
    }
    return errors;
}

/* ── 标定 ── */
void Gesture_Calibrate(uint8_t hand, uint8_t cal_type)
{
    Flex_Finger_t *h = hand ? Hand_Left : Hand_Right;

    for (uint8_t f = 0; f < 5; f++) {
        if (cal_type == 0) {  /* MIN: 记录伸直状态 */
            h[f].adc_min = h[f].current_raw;
        } else {              /* MAX: 记录弯曲状态 */
            h[f].adc_max = h[f].current_raw;
        }
    }
}
