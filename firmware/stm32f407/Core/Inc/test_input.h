#ifndef __TEST_INPUT_H
#define __TEST_INPUT_H

#include <stddef.h>
#include <stdint.h>

#include "jy61p.h"

#define TEST_INPUT_FLEX_COUNT 10U
#define TEST_INPUT_TIMEOUT_MS 5000U

typedef enum {
    TEST_INPUT_SOURCE_REAL = 0,
    TEST_INPUT_SOURCE_VIRTUAL = 1
} TestInput_Source_t;

typedef struct {
    uint8_t flex[TEST_INPUT_FLEX_COUNT];
    float angle[3];
    float acc[3];
    uint8_t acc_valid;
    uint32_t sequence;
} TestInput_Snapshot_t;

typedef enum {
    TEST_INPUT_NOT_COMMAND = 0,
    TEST_INPUT_HANDLED = 1
} TestInput_HandleResult_t;

void TestInput_Init(void);
TestInput_HandleResult_t TestInput_HandleLine(const char *line,
                                              uint32_t now_ms,
                                              char *response,
                                              size_t response_size);
uint8_t TestInput_Service(uint32_t now_ms);
TestInput_Source_t TestInput_GetSource(void);
uint8_t TestInput_HasAppliedSnapshot(void);
const TestInput_Snapshot_t *TestInput_GetAppliedSnapshot(void);
void TestInput_PublishMotion(JY61P_Data_t *target, uint32_t now_ms);

#endif /* __TEST_INPUT_H */
