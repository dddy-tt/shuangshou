#ifndef __GESTURE_H
#define __GESTURE_H

#include "stdint.h"

/* ── 手指三态阈值 ── */
#define BEND_THRESH_HALF  40   /* >40% 算半弯 */
#define BEND_THRESH_FULL  75   /* >75% 算全弯 */

/* ── 方向编码 ── */
#define DIR_NONE    0
#define DIR_UP      1
#define DIR_DOWN    2
#define DIR_LEFT    3
#define DIR_RIGHT   4
#define DIR_FORWARD 5
#define DIR_BACK    6

/* ── 工作模式 ── */
typedef enum {
    MODE_TRANSLATE = 0,  /* 翻译模式 */
    MODE_CONTROL   = 1,  /* IoT 控制模式 */
    MODE_REHAB     = 2,  /* AI 康复模式 */
} GestureMode_t;

/* ── 手势识别触发结果 ── */
typedef struct {
    uint8_t active;        /* 1=有新手势 */
    uint8_t file_index;    /* DFPlayer 文件编号 / MQTT 控制索引 */
    uint8_t is_ctrl;       /* ★ 1=控制指令 (不播 DFPlayer, 走 MQTT) */
    uint8_t ctrl_idx;      /* ★ 控制词表索引 */
    char    code_str[12];  /* 手势编码字符串（调试用）*/
} GestureResult_t;

/* ── 查表条目 ── */
typedef struct {
    char     code[12];     /* 手势编码，如 "00000111110" */
    uint16_t mp3_file;     /* 对应 MP3 文件编号 */
    uint8_t  is_dynamic;   /* 0=静态手势, 1=动态手势 */
} GestureEntry_t;

/* ── IoT 控制条目 ── */
typedef struct {
    char     code[12];     /* 手势编码 */
    char     topic[24];    /* MQTT Topic */
    char     payload[8];   /* MQTT Payload (ON/OFF) */
} GestureCtrl_t;

/* ── API ── */

void Gesture_Init(void);

/*
 * 核心判定：每 50ms 调用
 * 输入：10 指弯曲百分比 + 轨迹方向
 * 输出：GestureResult_t，active=1 表示触发了一个手势
 */
GestureResult_t Gesture_Evaluate(void);

/*
 * 获取当前 10 指编码（不含轨迹方向）
 * 用于教学模式、自定义手势录入
 */
void Gesture_GetCurrentCode(char *code_out);

/*
 * 计算当前手势与目标模板的逐指差异
 * target_code: 目标 10 字编码
 * 返回错误位掩码 (bit0=拇指, bit1=食指...)
 */
uint8_t Gesture_Compare(const char *target_code);

/*
 * 标定：记录当前 ADC 值作为 min 或 max
 * hand: 0=右手, 1=左手
 * cal_type: 0=MIN, 1=MAX
 */
void Gesture_Calibrate(uint8_t hand, uint8_t cal_type);

/* ── v3.0 新增: 模式管理 ── */
GestureMode_t Gesture_GetMode(void);
void Gesture_SetMode(GestureMode_t mode);

/* ── v3.1: 模式切换期间冻结手势输出 (防止 Hold 期间语音寄生触发) ── */
void Gesture_Freeze(void);
void Gesture_Unfreeze(void);
uint8_t Gesture_IsFrozen(void);

/* ── v3.0 新增: IoT 控制词表 (供 main.c 引用) ── */
extern const GestureCtrl_t ctrl_vocab[];
#define CTRL_VOCAB_COUNT  6U

#endif /* __GESTURE_H */
