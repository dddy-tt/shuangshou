#include <stdio.h>
#include <string.h>

#include "../Core/Inc/test_input.h"

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("FAIL: %s\n", message);                            \
            failures++;                                                \
        }                                                              \
    } while (0)

static const char *send_line(const char *line, uint32_t now_ms)
{
    static char response[80];
    TestInput_HandleResult_t handled;

    memset(response, 0, sizeof(response));
    handled = TestInput_HandleLine(line, now_ms, response,
                                   sizeof(response));
    CHECK(handled == TEST_INPUT_HANDLED, "TEST line must be handled");
    return response;
}

static void load_valid_snapshot(uint32_t now_ms)
{
    CHECK(strstr(send_line("TEST:FLEX|L1=0|L2=30|L3=70|L4=100|L5=40|"
                           "R1=50|R2=60|R3=70|R4=80|R5=90\r\n",
                           now_ms), "FLEX=OK") != NULL,
          "FLEX boundary values must be accepted");
    CHECK(strstr(send_line("TEST:IMU|R=10.00|P=-5.00|Y=2.00\n",
                           now_ms + 1U), "IMU=OK") != NULL,
          "valid IMU must be accepted");
    CHECK(strstr(send_line("TEST:ACC|X=0.00|Y=0.00|Z=1.00|VALID=1\n",
                           now_ms + 2U), "ACC=OK") != NULL,
          "valid ACC must be accepted");
}

int main(void)
{
    const TestInput_Snapshot_t *snapshot;
    TestInput_HandleResult_t result;
    char response[80];
    uint32_t sequence;

    TestInput_Init();
    CHECK(TestInput_GetSource() == TEST_INPUT_SOURCE_REAL,
          "power-on source must be REAL");
    result = TestInput_HandleLine("ALARM_ACK:1:2\n", 0U, response,
                                  sizeof(response));
    CHECK(result == TEST_INPUT_NOT_COMMAND,
          "non-TEST input must remain available to the real parser");

    CHECK(strstr(send_line("TEST:FLEX|L1=0|L2=0|L3=0|L4=0|L5=0|"
                           "R1=0|R2=0|R3=0|R4=0|R5=0\n", 1U),
                 "NOT_VIRTUAL") != NULL,
          "data before ENTER must be rejected");
    CHECK(strstr(send_line("TEST:ENTER\r\n", 10U), "MODE=VIRTUAL") != NULL,
          "ENTER must select VIRTUAL");
    CHECK(TestInput_GetSource() == TEST_INPUT_SOURCE_VIRTUAL,
          "source must report VIRTUAL after ENTER");
    CHECK(strstr(send_line("TEST:APPLY\n", 11U), "APPLY_INCOMPLETE") != NULL,
          "APPLY must require FLEX, IMU and ACC");

    CHECK(strstr(send_line("TEST:FLEX|L1=0|L2=30|L3=70|L4=101|L5=40|"
                           "R1=50|R2=60|R3=70|R4=80|R5=90\n", 12U),
                 "FLEX_INVALID") != NULL,
          "FLEX above 100 must be rejected");
    CHECK(strstr(send_line("TEST:FLEX|L1=0|L2=30|L3=70|L4=100|L5=40|"
                           "R1=50|R2=60|R3=70|R4=80\n", 13U),
                 "FLEX_INVALID") != NULL,
          "missing finger must be rejected");
    CHECK(strstr(send_line("TEST:IMU|R=181|P=0|Y=0\n", 14U),
                 "IMU_INVALID") != NULL,
          "out-of-range IMU must be rejected");
    CHECK(strstr(send_line("TEST:IMU|R=nan|P=0|Y=0\n", 15U),
                 "IMU_INVALID") != NULL,
          "NaN IMU must be rejected");
    CHECK(strstr(send_line("TEST:ACC|X=inf|Y=0|Z=1|VALID=1\n", 16U),
                 "ACC_INVALID") != NULL,
          "Inf ACC must be rejected");
    CHECK(strstr(send_line("TEST:ACC|X=0|Y=0|Z=1|VALID=2\n", 17U),
                 "ACC_INVALID") != NULL,
          "ACC valid must be 0 or 1");

    load_valid_snapshot(20U);
    CHECK(strstr(send_line("TEST:APPLY\n", 23U), "APPLY=OK") != NULL,
          "complete staged snapshot must apply");
    snapshot = TestInput_GetAppliedSnapshot();
    CHECK(snapshot != NULL, "applied snapshot must be available");
    CHECK(snapshot != NULL && snapshot->flex[0] == 0U &&
          snapshot->flex[1] == 30U && snapshot->flex[2] == 70U &&
          snapshot->flex[3] == 100U,
          "FLEX 0/30/70/100 must survive APPLY");
    CHECK(snapshot != NULL && snapshot->angle[0] == 10.0f &&
          snapshot->angle[1] == -5.0f && snapshot->angle[2] == 2.0f,
          "IMU values must survive APPLY");
    CHECK(snapshot != NULL && snapshot->acc[2] == 1.0f &&
          snapshot->acc_valid == 1U,
          "ACC and valid must survive APPLY");
    sequence = snapshot != NULL ? snapshot->sequence : 0U;

    CHECK(strstr(send_line("TEST:FLEX|L1=255|L2=0|L3=0|L4=0|L5=0|"
                           "R1=0|R2=0|R3=0|R4=0|R5=0\n", 24U),
                 "FLEX_INVALID") != NULL,
          "invalid update must be rejected after APPLY");
    snapshot = TestInput_GetAppliedSnapshot();
    CHECK(snapshot != NULL && snapshot->sequence == sequence &&
          snapshot->flex[3] == 100U,
          "invalid input must not overwrite the last applied snapshot");

    CHECK(TestInput_Service(23U + TEST_INPUT_TIMEOUT_MS - 1U) == 0U,
          "VIRTUAL must remain active before timeout");
    CHECK(TestInput_Service(23U + TEST_INPUT_TIMEOUT_MS) == 1U,
          "timeout must report automatic exit");
    CHECK(TestInput_GetSource() == TEST_INPUT_SOURCE_REAL,
          "timeout must restore REAL");
    CHECK(TestInput_GetAppliedSnapshot() == NULL,
          "timeout must invalidate virtual snapshot");

    CHECK(strstr(send_line("TEST:ENTER\n", 6000U), "MODE=VIRTUAL") != NULL,
          "second ENTER must work");
    CHECK(strstr(send_line("TEST:EXIT\n", 6001U), "MODE=REAL") != NULL,
          "EXIT must restore REAL");
    CHECK(TestInput_GetSource() == TEST_INPUT_SOURCE_REAL,
          "source must report REAL after EXIT");

    if (failures != 0) return 1;
    printf("test_input_test: all checks passed\n");
    return 0;
}
