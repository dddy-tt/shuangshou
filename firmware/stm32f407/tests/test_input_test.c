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
    JY61P_Data_t pending_motion;
    JY61P_Data_t pending_before;
    JY61P_Data_t motion;

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
    CHECK(TestInput_GetAppliedSnapshot() == NULL,
          "ENTER must not expose an applied snapshot before APPLY");
    CHECK(strstr(send_line("TEST:APPLY\n", 11U), "APPLY_INCOMPLETE") != NULL,
          "APPLY must require FLEX, IMU and ACC");

    memset(&pending_motion, 0, sizeof(pending_motion));
    pending_motion.angle[0] = 77.0f;
    pending_motion.acc[0] = -3.0f;
    pending_motion.online = 7U;
    pending_before = pending_motion;
    TestInput_PublishMotion(&pending_motion, 12U);
    CHECK(memcmp(&pending_motion, &pending_before, sizeof(pending_motion)) == 0,
          "motion publish must be a no-op before APPLY");

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
    CHECK(strstr(send_line("TEST:IMU|R=1e2|P=0|Y=0\n", 15U),
                 "IMU_INVALID") != NULL,
          "scientific-notation IMU must be rejected by strict decimal parser");
    CHECK(strstr(send_line("TEST:ACC|X=inf|Y=0|Z=1|VALID=1\n", 16U),
                 "ACC_INVALID") != NULL,
          "Inf ACC must be rejected");
    CHECK(strstr(send_line("TEST:ACC|X=1e0|Y=0|Z=1|VALID=1\n", 16U),
                 "ACC_INVALID") != NULL,
          "scientific-notation ACC must be rejected by strict decimal parser");
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

    memset(&motion, 0, sizeof(motion));
    TestInput_PublishMotion(&motion, 24U);
    CHECK(motion.angle[0] == 10.0f && motion.angle[1] == -5.0f &&
          motion.angle[2] == 2.0f,
          "first APPLY must publish all angle axes to formal JY structure");
    CHECK(motion.acc[0] == 0.0f && motion.acc[1] == 0.0f &&
          motion.acc[2] == 1.0f && motion.acc_raw[2] == 208,
          "first APPLY must preserve the existing JY61P raw scale");
    CHECK(motion.online != 0U && motion.angle_valid != 0U &&
          motion.acc_valid != 0U && motion.acc_updated_ms == 24U &&
          motion.angle_updated_ms == 24U,
          "first APPLY must publish formal JY validity and timestamp fields");

    CHECK(strstr(send_line("TEST:FLEX|L1=255|L2=0|L3=0|L4=0|L5=0|"
                           "R1=0|R2=0|R3=0|R4=0|R5=0\n", 24U),
                 "FLEX_INVALID") != NULL,
          "invalid update must be rejected after APPLY");
    snapshot = TestInput_GetAppliedSnapshot();
    CHECK(snapshot != NULL && snapshot->sequence == sequence &&
          snapshot->flex[3] == 100U,
          "invalid input must not overwrite the last applied snapshot");

    CHECK(strstr(send_line("TEST:FLEX|L1=100|L2=90|L3=80|L4=70|L5=60|"
                           "R1=50|R2=40|R3=30|R4=20|R5=10\n", 25U),
                 "FLEX=OK") != NULL,
          "second FLEX update must be accepted");
    CHECK(strstr(send_line("TEST:IMU|R=-12.50|P=3.25|Y=45.50\n", 26U),
                 "IMU=OK") != NULL,
          "second IMU update must be accepted");
    CHECK(strstr(send_line("TEST:ACC|X=-0.50|Y=0.25|Z=0.75|VALID=0\n", 27U),
                 "ACC=OK") != NULL,
          "second ACC update must be accepted");
    CHECK(strstr(send_line("TEST:APPLY\n", 28U), "APPLY=OK") != NULL,
          "second complete staged snapshot must apply");
    snapshot = TestInput_GetAppliedSnapshot();
    CHECK(snapshot != NULL && snapshot->sequence == sequence + 1U &&
          snapshot->angle[0] == -12.5f && snapshot->angle[1] == 3.25f &&
          snapshot->angle[2] == 45.5f,
          "second APPLY must replace all angle axes");
    CHECK(snapshot != NULL && snapshot->acc[0] == -0.5f &&
          snapshot->acc[1] == 0.25f && snapshot->acc[2] == 0.75f &&
          snapshot->acc_valid == 0U,
          "second APPLY must replace ACC and validity");
    TestInput_PublishMotion(&motion, 29U);
    CHECK(motion.angle[0] == -12.5f && motion.angle[1] == 3.25f &&
          motion.angle[2] == 45.5f && motion.acc[0] == -0.5f &&
          motion.acc[1] == 0.25f && motion.acc[2] == 0.75f &&
          motion.acc_raw[0] == -104 && motion.acc_raw[1] == 52 &&
          motion.acc_raw[2] == 156 && motion.acc_valid == 0U &&
          motion.acc_updated_ms == 29U && motion.angle_updated_ms == 29U,
          "second APPLY must update the formal JY structure");

    CHECK(TestInput_Service(28U + TEST_INPUT_TIMEOUT_MS - 1U) == 0U,
          "VIRTUAL must remain active before timeout");
    CHECK(TestInput_Service(28U + TEST_INPUT_TIMEOUT_MS) == 1U,
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
    CHECK(TestInput_GetAppliedSnapshot() == NULL,
          "EXIT must invalidate the applied virtual snapshot");

    if (failures != 0) return 1;
    printf("test_input_test: all checks passed\n");
    return 0;
}
