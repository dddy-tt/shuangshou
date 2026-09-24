#include "test_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_INPUT_IMU_LIMIT_DEG 180.0f
#define TEST_INPUT_ACC_LIMIT_G    16.0f

typedef struct {
    TestInput_Source_t source;
    TestInput_Snapshot_t staged;
    TestInput_Snapshot_t applied;
    uint8_t staged_flex;
    uint8_t staged_imu;
    uint8_t staged_acc;
    uint8_t has_applied;
    uint32_t last_activity_ms;
} TestInput_State_t;

static TestInput_State_t test_state;

static void test_input_reply(char *out, size_t out_size, const char *text)
{
    if (out == NULL || out_size == 0U) return;
    (void)snprintf(out, out_size, "%s\r\n", text);
}

static uint8_t test_input_line_ended(const char *line, int consumed)
{
    const char *tail;

    if (line == NULL || consumed < 0) return 0U;
    tail = &line[consumed];
    while (*tail == '\r' || *tail == '\n') tail++;
    return (*tail == '\0') ? 1U : 0U;
}

static uint8_t test_input_in_range(float value, float limit)
{
    return (isfinite(value) && value >= -limit && value <= limit) ? 1U : 0U;
}

static uint8_t test_input_parse_flex(const char *line,
                                     TestInput_Snapshot_t *target)
{
    unsigned values[TEST_INPUT_FLEX_COUNT];
    int consumed = -1;
    int count;
    uint8_t i;

    count = sscanf(line,
                   "TEST:FLEX|L1=%u|L2=%u|L3=%u|L4=%u|L5=%u|"
                   "R1=%u|R2=%u|R3=%u|R4=%u|R5=%u%n",
                   &values[0], &values[1], &values[2], &values[3],
                   &values[4], &values[5], &values[6], &values[7],
                   &values[8], &values[9], &consumed);
    if (count != (int)TEST_INPUT_FLEX_COUNT ||
        test_input_line_ended(line, consumed) == 0U) {
        return 0U;
    }
    for (i = 0U; i < TEST_INPUT_FLEX_COUNT; i++) {
        if (values[i] > 100U) return 0U;
    }
    for (i = 0U; i < TEST_INPUT_FLEX_COUNT; i++) {
        target->flex[i] = (uint8_t)values[i];
    }
    return 1U;
}

static uint8_t test_input_parse_imu(const char *line,
                                    TestInput_Snapshot_t *target)
{
    float roll;
    float pitch;
    float yaw;
    int consumed = -1;

    if (sscanf(line, "TEST:IMU|R=%f|P=%f|Y=%f%n",
               &roll, &pitch, &yaw, &consumed) != 3 ||
        test_input_line_ended(line, consumed) == 0U ||
        test_input_in_range(roll, TEST_INPUT_IMU_LIMIT_DEG) == 0U ||
        test_input_in_range(pitch, TEST_INPUT_IMU_LIMIT_DEG) == 0U ||
        test_input_in_range(yaw, TEST_INPUT_IMU_LIMIT_DEG) == 0U) {
        return 0U;
    }
    target->angle[0] = roll;
    target->angle[1] = pitch;
    target->angle[2] = yaw;
    return 1U;
}

static uint8_t test_input_parse_acc(const char *line,
                                    TestInput_Snapshot_t *target)
{
    float x;
    float y;
    float z;
    unsigned valid;
    int consumed = -1;

    if (sscanf(line, "TEST:ACC|X=%f|Y=%f|Z=%f|VALID=%u%n",
               &x, &y, &z, &valid, &consumed) != 4 ||
        test_input_line_ended(line, consumed) == 0U ||
        valid > 1U ||
        test_input_in_range(x, TEST_INPUT_ACC_LIMIT_G) == 0U ||
        test_input_in_range(y, TEST_INPUT_ACC_LIMIT_G) == 0U ||
        test_input_in_range(z, TEST_INPUT_ACC_LIMIT_G) == 0U) {
        return 0U;
    }
    target->acc[0] = x;
    target->acc[1] = y;
    target->acc[2] = z;
    target->acc_valid = (uint8_t)valid;
    return 1U;
}

void TestInput_Init(void)
{
    memset(&test_state, 0, sizeof(test_state));
    test_state.source = TEST_INPUT_SOURCE_REAL;
}

TestInput_HandleResult_t TestInput_HandleLine(const char *line,
                                              uint32_t now_ms,
                                              char *response,
                                              size_t response_size)
{
    if (line == NULL || strncmp(line, "TEST:", 5U) != 0) {
        return TEST_INPUT_NOT_COMMAND;
    }

    if (strncmp(line, "TEST:ENTER", 10U) == 0 &&
        test_input_line_ended(line, 10) != 0U) {
        memset(&test_state.staged, 0, sizeof(test_state.staged));
        test_state.staged_flex = 0U;
        test_state.staged_imu = 0U;
        test_state.staged_acc = 0U;
        test_state.has_applied = 0U;
        test_state.source = TEST_INPUT_SOURCE_VIRTUAL;
        test_state.last_activity_ms = now_ms;
        test_input_reply(response, response_size, "[TEST] MODE=VIRTUAL");
        return TEST_INPUT_HANDLED;
    }

    if (strncmp(line, "TEST:EXIT", 9U) == 0 &&
        test_input_line_ended(line, 9) != 0U) {
        test_state.source = TEST_INPUT_SOURCE_REAL;
        test_state.has_applied = 0U;
        test_input_reply(response, response_size, "[TEST] MODE=REAL");
        return TEST_INPUT_HANDLED;
    }

    if (test_state.source != TEST_INPUT_SOURCE_VIRTUAL) {
        test_input_reply(response, response_size,
                         "[TEST] ERROR=NOT_VIRTUAL");
        return TEST_INPUT_HANDLED;
    }

    if (strncmp(line, "TEST:FLEX|", 10U) == 0) {
        TestInput_Snapshot_t candidate = test_state.staged;
        if (test_input_parse_flex(line, &candidate) == 0U) {
            test_input_reply(response, response_size,
                             "[TEST] ERROR=FLEX_INVALID");
            return TEST_INPUT_HANDLED;
        }
        test_state.staged = candidate;
        test_state.staged_flex = 1U;
        test_state.last_activity_ms = now_ms;
        test_input_reply(response, response_size, "[TEST] FLEX=OK");
        return TEST_INPUT_HANDLED;
    }

    if (strncmp(line, "TEST:IMU|", 9U) == 0) {
        TestInput_Snapshot_t candidate = test_state.staged;
        if (test_input_parse_imu(line, &candidate) == 0U) {
            test_input_reply(response, response_size,
                             "[TEST] ERROR=IMU_INVALID");
            return TEST_INPUT_HANDLED;
        }
        test_state.staged = candidate;
        test_state.staged_imu = 1U;
        test_state.last_activity_ms = now_ms;
        test_input_reply(response, response_size, "[TEST] IMU=OK");
        return TEST_INPUT_HANDLED;
    }

    if (strncmp(line, "TEST:ACC|", 9U) == 0) {
        TestInput_Snapshot_t candidate = test_state.staged;
        if (test_input_parse_acc(line, &candidate) == 0U) {
            test_input_reply(response, response_size,
                             "[TEST] ERROR=ACC_INVALID");
            return TEST_INPUT_HANDLED;
        }
        test_state.staged = candidate;
        test_state.staged_acc = 1U;
        test_state.last_activity_ms = now_ms;
        test_input_reply(response, response_size, "[TEST] ACC=OK");
        return TEST_INPUT_HANDLED;
    }

    if (strncmp(line, "TEST:APPLY", 10U) == 0 &&
        test_input_line_ended(line, 10) != 0U) {
        if (test_state.staged_flex == 0U ||
            test_state.staged_imu == 0U ||
            test_state.staged_acc == 0U) {
            test_input_reply(response, response_size,
                             "[TEST] ERROR=APPLY_INCOMPLETE");
            return TEST_INPUT_HANDLED;
        }
        test_state.staged.sequence = test_state.applied.sequence + 1U;
        test_state.applied = test_state.staged;
        test_state.has_applied = 1U;
        test_state.last_activity_ms = now_ms;
        test_input_reply(response, response_size, "[TEST] APPLY=OK");
        return TEST_INPUT_HANDLED;
    }

    test_input_reply(response, response_size, "[TEST] ERROR=UNKNOWN");
    return TEST_INPUT_HANDLED;
}

uint8_t TestInput_Service(uint32_t now_ms)
{
    if (test_state.source == TEST_INPUT_SOURCE_VIRTUAL &&
        (uint32_t)(now_ms - test_state.last_activity_ms) >=
        TEST_INPUT_TIMEOUT_MS) {
        test_state.source = TEST_INPUT_SOURCE_REAL;
        test_state.has_applied = 0U;
        return 1U;
    }
    return 0U;
}

TestInput_Source_t TestInput_GetSource(void)
{
    return test_state.source;
}

uint8_t TestInput_HasAppliedSnapshot(void)
{
    return test_state.has_applied;
}

const TestInput_Snapshot_t *TestInput_GetAppliedSnapshot(void)
{
    return (test_state.has_applied != 0U) ? &test_state.applied : NULL;
}
