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

static uint8_t test_input_line_ended_at(const char *tail)
{
    if (tail == NULL) return 0U;
    while (*tail == '\r' || *tail == '\n') tail++;
    return (*tail == '\0') ? 1U : 0U;
}

static uint8_t test_input_line_ended(const char *line, int consumed)
{
    if (line == NULL || consumed < 0) return 0U;
    return test_input_line_ended_at(&line[consumed]);
}

static uint8_t test_input_in_range(float value, float limit)
{
    return (isfinite(value) && value >= -limit && value <= limit) ? 1U : 0U;
}

static uint8_t test_input_match(const char **cursor, const char *literal)
{
    size_t length;

    if (cursor == NULL || *cursor == NULL || literal == NULL) return 0U;
    length = strlen(literal);
    if (strncmp(*cursor, literal, length) != 0) return 0U;
    *cursor += length;
    return 1U;
}

static uint8_t test_input_parse_uint(const char **cursor,
                                     uint32_t maximum,
                                     uint32_t *value)
{
    const char *p;
    uint32_t parsed = 0U;
    uint8_t saw_digit = 0U;

    if (cursor == NULL || *cursor == NULL || value == NULL) return 0U;
    p = *cursor;
    while (*p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p - '0');

        if (parsed > (maximum / 10U) ||
            (parsed == (maximum / 10U) && digit > (maximum % 10U))) {
            return 0U;
        }
        parsed = parsed * 10U + digit;
        saw_digit = 1U;
        p++;
    }
    if (saw_digit == 0U) return 0U;
    *cursor = p;
    *value = parsed;
    return 1U;
}

/*
 * Parse only the protocol's fixed-point decimal grammar:
 * [+-]?[0-9]+(\.[0-9]+)?
 *
 * In particular, do not use scanf/strtof here.  The ARMCC5 MicroLIB target
 * can report a successful %f conversion while storing a non-sensical float.
 * The parser deliberately does not accept exponent notation, NaN, or Inf.
 */
static uint8_t test_input_parse_decimal(const char **cursor, float *value)
{
    const char *p;
    float parsed = 0.0f;
    float fraction_scale = 0.1f;
    uint8_t negative = 0U;
    uint8_t integer_digit = 0U;
    uint8_t fraction_digit = 0U;

    if (cursor == NULL || *cursor == NULL || value == NULL) return 0U;
    p = *cursor;
    if (*p == '+' || *p == '-') {
        negative = (*p == '-') ? 1U : 0U;
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        parsed = parsed * 10.0f + (float)(*p - '0');
        if (isfinite(parsed) == 0) return 0U;
        integer_digit = 1U;
        p++;
    }

    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            parsed += (float)(*p - '0') * fraction_scale;
            fraction_scale *= 0.1f;
            if (isfinite(parsed) == 0 ||
                isfinite(fraction_scale) == 0) {
                return 0U;
            }
            fraction_digit = 1U;
            p++;
        }
        if (fraction_digit == 0U) return 0U;
    }

    if (integer_digit == 0U || *p == 'e' || *p == 'E' ||
        isfinite(parsed) == 0) {
        return 0U;
    }
    *value = (negative != 0U) ? -parsed : parsed;
    *cursor = p;
    return isfinite(*value) ? 1U : 0U;
}

static uint8_t test_input_parse_flex(const char *line,
                                     TestInput_Snapshot_t *target)
{
    static const char *const labels[TEST_INPUT_FLEX_COUNT] = {
        "L1=", "L2=", "L3=", "L4=", "L5=",
        "R1=", "R2=", "R3=", "R4=", "R5="
    };
    const char *cursor = line;
    uint32_t values[TEST_INPUT_FLEX_COUNT];
    uint8_t i;

    if (test_input_match(&cursor, "TEST:FLEX|") == 0U) return 0U;
    for (i = 0U; i < TEST_INPUT_FLEX_COUNT; i++) {
        if (test_input_match(&cursor, labels[i]) == 0U ||
            test_input_parse_uint(&cursor, 100U, &values[i]) == 0U) {
            return 0U;
        }
        if (i + 1U < TEST_INPUT_FLEX_COUNT &&
            test_input_match(&cursor, "|") == 0U) {
            return 0U;
        }
    }
    if (test_input_line_ended_at(cursor) == 0U) {
        return 0U;
    }
    for (i = 0U; i < TEST_INPUT_FLEX_COUNT; i++) {
        target->flex[i] = (uint8_t)values[i];
    }
    return 1U;
}

static uint8_t test_input_parse_imu(const char *line,
                                    TestInput_Snapshot_t *target)
{
    const char *cursor = line;
    float roll;
    float pitch;
    float yaw;

    if (test_input_match(&cursor, "TEST:IMU|R=") == 0U ||
        test_input_parse_decimal(&cursor, &roll) == 0U ||
        test_input_match(&cursor, "|P=") == 0U ||
        test_input_parse_decimal(&cursor, &pitch) == 0U ||
        test_input_match(&cursor, "|Y=") == 0U ||
        test_input_parse_decimal(&cursor, &yaw) == 0U ||
        test_input_line_ended_at(cursor) == 0U ||
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
    const char *cursor = line;
    float x;
    float y;
    float z;
    uint32_t valid;

    if (test_input_match(&cursor, "TEST:ACC|X=") == 0U ||
        test_input_parse_decimal(&cursor, &x) == 0U ||
        test_input_match(&cursor, "|Y=") == 0U ||
        test_input_parse_decimal(&cursor, &y) == 0U ||
        test_input_match(&cursor, "|Z=") == 0U ||
        test_input_parse_decimal(&cursor, &z) == 0U ||
        test_input_match(&cursor, "|VALID=") == 0U ||
        test_input_parse_uint(&cursor, 1U, &valid) == 0U ||
        test_input_line_ended_at(cursor) == 0U ||
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

static int16_t test_input_float_to_raw(float value, float scale)
{
    float raw = value / scale;

    if (raw > 32767.0f) return 32767;
    if (raw < -32768.0f) return -32768;
    return (int16_t)raw;
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

void TestInput_PublishMotion(JY61P_Data_t *target, uint32_t now_ms)
{
    const TestInput_Snapshot_t *snapshot = TestInput_GetAppliedSnapshot();
    uint8_t i;

    if (target == NULL || snapshot == NULL) return;

    for (i = 0U; i < 3U; i++) {
        target->acc[i] = snapshot->acc[i];
        target->acc_raw[i] = test_input_float_to_raw(
            snapshot->acc[i], JY61P_ACC_SCALE);
        target->gyro[i] = 0.0f;
        target->gyro_raw[i] = 0;
        target->angle[i] = snapshot->angle[i];
        target->angle_raw[i] = test_input_float_to_raw(
            snapshot->angle[i], JY61P_ANGLE_SCALE);
    }
    target->online = 1U;
    target->error_streak = 0U;
    target->last_error = 0U;
    target->acc_valid = snapshot->acc_valid;
    target->angle_valid = 1U;
    target->acc_sample_seen = 1U;
    target->angle_sample_seen = 1U;
    target->angle_zero_streak = 0U;
    target->acc_updated_ms = now_ms;
    target->angle_updated_ms = now_ms;
}
