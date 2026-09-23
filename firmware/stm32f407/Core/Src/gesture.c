/**
  ******************************************************************************
  * @file           : gesture.c
  * @brief          : Hand gesture recognition core for the dual-glove project
  * @date           : 2026-06-29
  ******************************************************************************
  */

#include "gesture.h"
#include "flex_sensor.h"
#include "jy61p.h"
#include "string.h"
#include "stdio.h"

extern volatile uint32_t sys_tick_ms;

static uint32_t app_now_ms(void)
{
    return sys_tick_ms;
}

#define JERK_WINDOW            20U
#define STABLE_THRESH_HIGH     1000
#define STABLE_THRESH_MED      1500
#define STABLE_THRESH_LOW      2200
#define GYRO_ROTATE_THRESH_SQ  3600.0f
#define STABLE_TIME_MS         500U
#define TRAJ_TIMEOUT_MS        1500U
#define COOLDOWN_WINDOW_MS     250U
#define COOLDOWN_MS            800U
#define ROTATION_HOLD_MS       200U

static int16_t ax_hist[JERK_WINDOW], ay_hist[JERK_WINDOW], az_hist[JERK_WINDOW];
static int16_t gx_hist[JERK_WINDOW], gy_hist[JERK_WINDOW], gz_hist[JERK_WINDOW];
static uint8_t jerk_idx = 0;
static uint8_t jerk_warmup = 0;
static uint8_t jerk_dir = DIR_NONE;

static uint8_t rotation_frozen = 0;
static uint32_t rotation_frozen_since = 0;

typedef enum {
    ST_WAIT_STABLE = 0,
    ST_WAIT_TRAJ,
    ST_ARMED,
    ST_LATCH,
    ST_COOLDOWN
} GestureState_t;

static GestureState_t gs_state = ST_WAIT_STABLE;
static char stable_code[12] = "00000000000";
static uint32_t stable_start = 0;
static uint32_t cooldown_start = 0;
static uint32_t latch_start = 0;
static char latch_best_code[12];
static uint8_t latch_best_file = 0;
static uint8_t latch_has_match = 0;
static char last_triggered[12] = "";

static const GestureEntry_t vocab[] = {
    {"00000111110",  1, 0},
    {"00000000005",  2, 1},
    {"11000000000",  3, 0},
    {"11000000004",  4, 1},
    {"00000222220",  5, 0},
    {"22222000004", 10, 1},
    {"00000222224", 11, 1},
    {"00000122220", 12, 0},
    {"22222000000", 13, 0},
    {"00100001006", 14, 1},
    {"00100001003", 20, 1},
    {"22222222221", 21, 1},
};
#define VOCAB_SIZE  (sizeof(vocab) / sizeof(vocab[0]))

const GestureCtrl_t ctrl_vocab[] = {
    {"00000110000", "glove/light",  "ON"},
    {"00000110001", "glove/light",  "OFF"},
    {"00000220000", "glove/fan",    "ON"},
    {"00000220001", "glove/fan",    "OFF"},
    {"00000120000", "glove/socket", "ON"},
    {"00000120001", "glove/socket", "OFF"},
};
#define CTRL_VOCAB_SIZE  (sizeof(ctrl_vocab) / sizeof(ctrl_vocab[0]))

static GestureMode_t g_mode = MODE_TRANSLATE;
static GestureSensitivity_t g_sensitivity = GESTURE_SENS_MED;
static uint8_t g_frozen = 0;

static void encode_fingers(char *code)
{
    for (uint8_t h = 0; h < 2; h++) {
        for (uint8_t f = 0; f < 5; f++) {
            int16_t pct = (int16_t)Flex_GetPercent(h, f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;

            if (pct > (int16_t)BEND_THRESH_FULL) {
                code[h * 5 + f] = '2';
            } else if (pct > (int16_t)BEND_THRESH_HALF) {
                code[h * 5 + f] = '1';
            } else {
                code[h * 5 + f] = '0';
            }
        }
    }
    code[10] = '\0';
}

static int32_t gesture_get_stable_thresh(void)
{
    switch (g_sensitivity) {
    case GESTURE_SENS_HIGH:
        return STABLE_THRESH_HIGH;
    case GESTURE_SENS_LOW:
        return STABLE_THRESH_LOW;
    case GESTURE_SENS_MED:
    default:
        return STABLE_THRESH_MED;
    }
}

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

    if (jerk_warmup < JERK_WINDOW) {
        jerk_warmup++;
        return DIR_NONE;
    }

    {
        float gyro_energy = r->gyro[0] * r->gyro[0]
                          + r->gyro[1] * r->gyro[1]
                          + r->gyro[2] * r->gyro[2];
        if (gyro_energy > GYRO_ROTATE_THRESH_SQ) {
            if (!rotation_frozen) {
                rotation_frozen = 1U;
                rotation_frozen_since = app_now_ms();
            }
            return DIR_NONE;
        }
    }

    if (rotation_frozen) {
        uint32_t now = app_now_ms();
        if (now - rotation_frozen_since > ROTATION_HOLD_MS) {
            rotation_frozen = 0U;
        } else {
            return DIR_NONE;
        }
    }

    {
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

        if (jerk_x + jerk_y + jerk_z < gesture_get_stable_thresh()) {
            return DIR_NONE;
        }

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
    }

    return DIR_NONE;
}

static uint8_t lookup_vocab(const char *full_code, uint16_t *file_out)
{
    for (uint16_t i = 0; i < VOCAB_SIZE; i++) {
        if (strcmp(full_code, vocab[i].code) == 0) {
            *file_out = vocab[i].mp3_file;
            return 1U;
        }
    }
    return 0U;
}

void Gesture_Init(void)
{
    memset(ax_hist, 0, sizeof(ax_hist));
    memset(ay_hist, 0, sizeof(ay_hist));
    memset(az_hist, 0, sizeof(az_hist));
    memset(gx_hist, 0, sizeof(gx_hist));
    memset(gy_hist, 0, sizeof(gy_hist));
    memset(gz_hist, 0, sizeof(gz_hist));
    memset(stable_code, 0, sizeof(stable_code));
    memset(last_triggered, 0, sizeof(last_triggered));
    memset(latch_best_code, 0, sizeof(latch_best_code));

    jerk_idx = 0;
    jerk_warmup = 0;
    jerk_dir = DIR_NONE;
    rotation_frozen = 0;
    rotation_frozen_since = 0;
    gs_state = ST_WAIT_STABLE;
    stable_start = 0;
    cooldown_start = 0;
    latch_start = 0;
    latch_best_file = 0;
    latch_has_match = 0;
}

GestureResult_t Gesture_Evaluate(void)
{
    GestureResult_t res = {0};
    uint32_t now = app_now_ms();
    uint8_t dir = DIR_NONE;
    char cur_code[11];

    if (g_frozen) {
        return res;
    }

    encode_fingers(cur_code);
    dir = jerk_detect();

    switch (gs_state) {
    case ST_WAIT_STABLE:
        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
        } else if ((now - stable_start) > STABLE_TIME_MS) {
            gs_state = ST_WAIT_TRAJ;
        }
        break;

    case ST_WAIT_TRAJ:
        if (dir != DIR_NONE) {
            jerk_dir = dir;
            gs_state = ST_ARMED;
        } else if ((now - stable_start) > TRAJ_TIMEOUT_MS) {
            jerk_dir = DIR_NONE;
            gs_state = ST_ARMED;
        }

        if (strcmp(cur_code, stable_code) != 0) {
            strcpy(stable_code, cur_code);
            stable_start = now;
            gs_state = ST_WAIT_STABLE;
        }
        break;

    case ST_ARMED:
    {
        char full_code[12];
        snprintf(full_code, sizeof(full_code), "%s%c",
                 stable_code,
                 (jerk_dir == DIR_NONE) ? '0' : (char)('0' + jerk_dir));

        if (strcmp(full_code, last_triggered) != 0) {
            uint16_t file_tmp = 0;
            if (lookup_vocab(full_code, &file_tmp)) {
                strcpy(latch_best_code, full_code);
                latch_best_file = (uint8_t)file_tmp;
                latch_has_match = 1U;
            } else {
                strcpy(latch_best_code, full_code);
                latch_has_match = 0U;
            }
        }

        latch_start = now;
        gs_state = ST_LATCH;
        break;
    }

    case ST_LATCH:
    {
        char latch_code[12];
        snprintf(latch_code, sizeof(latch_code), "%s%c",
                 cur_code,
                 (dir == DIR_NONE) ? '0' : (char)('0' + dir));

        if (g_mode == MODE_CONTROL) {
            for (uint16_t i = 0; i < CTRL_VOCAB_SIZE; i++) {
                if (strcmp(latch_code, ctrl_vocab[i].code) == 0) {
                    latch_has_match = 1U;
                    latch_best_file = (uint8_t)i;
                    strcpy(latch_best_code, latch_code);
                    break;
                }
            }
        } else {
            uint16_t file_tmp = 0;
            if (lookup_vocab(latch_code, &file_tmp)) {
                if (strcmp(latch_code, latch_best_code) != 0) {
                    strcpy(latch_best_code, latch_code);
                    latch_best_file = (uint8_t)file_tmp;
                    latch_has_match = 1U;
                }
            }
        }

        if ((now - latch_start) > COOLDOWN_WINDOW_MS) {
            if (latch_has_match) {
                res.active = 1U;
                res.file_index = latch_best_file;
                res.is_ctrl = (g_mode == MODE_CONTROL) ? 1U : 0U;
                res.ctrl_idx = (g_mode == MODE_CONTROL) ? latch_best_file : 0U;
                strncpy(res.code_str, latch_best_code, 11);
                res.code_str[11] = '\0';
                strcpy(last_triggered, latch_best_code);
            } else {
                strcpy(last_triggered, latch_best_code);
            }

            cooldown_start = now;
            gs_state = ST_COOLDOWN;
        }
        break;
    }

    case ST_COOLDOWN:
        if ((now - cooldown_start) > COOLDOWN_MS) {
            gs_state = ST_WAIT_STABLE;
            memset(stable_code, 0, sizeof(stable_code));
            stable_start = now;
            latch_has_match = 0U;
            latch_best_file = 0U;
        }
        break;
    }

    return res;
}

void Gesture_GetCurrentCode(char *code_out)
{
    encode_fingers(code_out);
}

uint16_t Gesture_Compare(const char *target_code)
{
    uint16_t errors = 0;
    char cur[11];

    encode_fingers(cur);
    for (uint8_t i = 0; i < 10; i++) {
        if (cur[i] != target_code[i]) {
            errors |= (uint16_t)(1U << i);
        }
    }
    return errors;
}

GestureMode_t Gesture_GetMode(void) { return g_mode; }
void Gesture_SetMode(GestureMode_t mode) { g_mode = mode; }
GestureSensitivity_t Gesture_GetSensitivity(void) { return g_sensitivity; }
void Gesture_SetSensitivity(GestureSensitivity_t level)
{
    if (level > GESTURE_SENS_LOW) {
        level = GESTURE_SENS_MED;
    }
    g_sensitivity = level;
}

void Gesture_Freeze(void) { g_frozen = 1U; }
void Gesture_Unfreeze(void) { g_frozen = 0U; }
uint8_t Gesture_IsFrozen(void) { return g_frozen; }

void Gesture_Calibrate(uint8_t hand, uint8_t cal_type)
{
    Flex_Finger_t *h = hand ? Hand_Left : Hand_Right;
    for (uint8_t f = 0; f < 5; f++) {
        uint16_t raw = h[f].current_raw;
        if (raw > 4095U) {
            raw = 4095U;
        }
        if (cal_type == 0U) {
            h[f].adc_min = raw;
        } else {
            h[f].adc_max = raw;
        }
    }
}
