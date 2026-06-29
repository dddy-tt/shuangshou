#ifndef __GESTURE_H
#define __GESTURE_H

#include "stdint.h"

#define BEND_THRESH_HALF  40
#define BEND_THRESH_FULL  75

#define DIR_NONE    0
#define DIR_UP      1
#define DIR_DOWN    2
#define DIR_LEFT    3
#define DIR_RIGHT   4
#define DIR_FORWARD 5
#define DIR_BACK    6

typedef enum {
    MODE_TRANSLATE = 0,
    MODE_CONTROL   = 1,
    MODE_REHAB     = 2,
} GestureMode_t;

typedef enum {
    GESTURE_SENS_HIGH = 0,
    GESTURE_SENS_MED  = 1,
    GESTURE_SENS_LOW  = 2,
} GestureSensitivity_t;

typedef struct {
    uint8_t active;
    uint8_t file_index;
    uint8_t is_ctrl;
    uint8_t ctrl_idx;
    char    code_str[12];
} GestureResult_t;

typedef struct {
    char     code[12];
    uint16_t mp3_file;
    uint8_t  is_dynamic;
} GestureEntry_t;

typedef struct {
    char     code[12];
    char     topic[24];
    char     payload[8];
} GestureCtrl_t;

void Gesture_Init(void);
GestureResult_t Gesture_Evaluate(void);
void Gesture_GetCurrentCode(char *code_out);
uint16_t Gesture_Compare(const char *target_code);
void Gesture_Calibrate(uint8_t hand, uint8_t cal_type);
GestureMode_t Gesture_GetMode(void);
void Gesture_SetMode(GestureMode_t mode);
GestureSensitivity_t Gesture_GetSensitivity(void);
void Gesture_SetSensitivity(GestureSensitivity_t level);
void Gesture_Freeze(void);
void Gesture_Unfreeze(void);
uint8_t Gesture_IsFrozen(void);

extern const GestureCtrl_t ctrl_vocab[];
#define CTRL_VOCAB_COUNT  6U

#endif /* __GESTURE_H */
