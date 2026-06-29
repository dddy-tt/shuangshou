#ifndef __FLEX_SENSOR_H
#define __FLEX_SENSOR_H

#include "stdint.h"

#define HISTORY_SIZE 10   /* 滑动窗口大小（痉挛检测用）*/
#define FINGER_NUM   5    /* 一只手 5 指 */

/* ── 手指索引 ── */
typedef enum {
    THUMB = 0,
    INDEX,
    MIDDLE,
    RING,
    PINKY,
    FINGER_COUNT = 5
} Finger_t;

/* ── 单指数据结构 ── */
typedef struct {
    uint16_t current_raw;        /* EMA 滤波后的当前 ADC 值 */
    uint8_t  pos_percent;        /* 弯曲百分比 0~100 */

    uint8_t history_buffer[HISTORY_SIZE];  /* 用于痉挛检测的环形缓冲 */
    uint8_t history_index;

    /* 标定极值（由蓝牙指令或本地标定设置）*/
    uint16_t adc_min;            /* 伸直时 ADC 值 */
    uint16_t adc_max;            /* 完全弯曲时 ADC 值 */
} Flex_Finger_t;

/* ── 全局双手数据 ── */
extern Flex_Finger_t Hand_Right[FINGER_NUM];
extern Flex_Finger_t Hand_Left[FINGER_NUM];

/* ── 外部 DMA 缓冲（main.c 声明，此处引用） ── */
extern volatile uint16_t adc1_buf[FINGER_NUM];  /* ADC1: 右手 PA0~PA4 */
extern volatile uint16_t adc2_buf[FINGER_NUM];  /* ADC2: 左手 PA5~PA7,PB0~PB1 */

/* ── API ── */

/* 初始化：极值反向初始化（迫使系统自适应）*/
void Flex_Init(void);

/*
 * 更新数据：ADC DMA 缓冲 → EMA 滤波 → 百分比映射
 * 调用频率：50Hz（每 20ms）
 */
void Flex_Update(void);

/*
 * 按手和手指获取百分比
 */
uint8_t Flex_GetPercent(uint8_t hand /* 0=右, 1=左 */, uint8_t finger);

/*
 * 痉挛检测：滑动窗口全变差（Total Variation）
 * 返回 0=正常, 1=手指抽动
 */
uint8_t Flex_CheckSpasm(uint8_t hand, uint8_t finger);

/*
 * 计算当前手势与模板的平方距离（教学模式用）
 */
uint32_t Flex_CalcSquaredDistance(uint8_t hand, uint8_t *standard_pose);

#endif /* __FLEX_SENSOR_H */
