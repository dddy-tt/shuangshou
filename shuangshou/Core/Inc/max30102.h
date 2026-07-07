/**
  ******************************************************************************
  * @file           : max30102.h
  * @brief          : MAX30102 心率血氧传感器驱动 — 硬件 I2C
  ******************************************************************************
  */

#ifndef __MAX30102_H
#define __MAX30102_H

#include "stdint.h"

#define MAX30102_I2C_ADDR       0xAEU  /* 7-bit: 0x57, 8-bit 写地址: 0xAE */
#define MAX30102_PART_ID_VAL    0x15U

#define MAX30102_INT_STATUS1    0x00U
#define MAX30102_INT_STATUS2    0x01U
#define MAX30102_INT_ENABLE1    0x02U
#define MAX30102_INT_ENABLE2    0x03U
#define MAX30102_FIFO_WR_PTR    0x04U
#define MAX30102_OVF_COUNTER    0x05U
#define MAX30102_FIFO_RD_PTR    0x06U
#define MAX30102_FIFO_DATA      0x07U
#define MAX30102_FIFO_CONFIG    0x08U
#define MAX30102_MODE_CONFIG    0x09U
#define MAX30102_SPO2_CONFIG    0x0AU
#define MAX30102_LED1_PA        0x0CU
#define MAX30102_LED2_PA        0x0DU
#define MAX30102_PILOT_PA       0x10U
#define MAX30102_MULTI_LED_CTRL1 0x11U
#define MAX30102_MULTI_LED_CTRL2 0x12U
#define MAX30102_TEMP_INTEGER   0x1FU
#define MAX30102_TEMP_FRACTION  0x20U
#define MAX30102_TEMP_CONFIG    0x21U
#define MAX30102_REV_ID         0xFEU
#define MAX30102_PART_ID        0xFFU

#define MAX30102_MODE_HR        0x02U
#define MAX30102_MODE_SPO2      0x03U
#define MAX30102_MODE_MULTI     0x07U
#define MAX30102_SHDN           0x80U
#define MAX30102_RESET          0x40U

#define MAX30102_SPO2_CFG_100HZ 0x1FU

#define MAX30102_FIFO_ROLLOVER  0x10U
#define MAX30102_FIFO_AEMPTY(n) ((n) & 0x0FU)
#define MAX30102_FIFO_DEPTH     32U
#define MAX30102_FIFO_SAMPLE_BYTES 6U

#define MAX30102_IR_MIN_VALID   5000L
#define MAX30102_IR_MAX_VALID   200000L

#define MAX30102_BUF_LEN        20U

typedef struct {
    uint32_t ir;
    uint32_t red;
    uint8_t  valid;
    uint32_t tick_ms;
} MAX30102_Sample_t;

extern MAX30102_Sample_t max30102_buf[MAX30102_BUF_LEN];
extern uint8_t           max30102_buf_idx;
extern uint8_t           max30102_ready;

uint8_t MAX30102_Init(void);
void    MAX30102_ReadFIFO(void);
uint8_t MAX30102_GetHR(void);
uint8_t MAX30102_GetSpO2(void);
void    MAX30102_ResetHistory(void);
uint8_t MAX30102_IsOnline(void);
void    MAX30102_ProcessTick(void);

#endif /* __MAX30102_H */
