#include "alarm.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("FAIL: %s\n", message);                            \
            failures++;                                                \
        }                                                              \
    } while (0)

static Alarm_Sample_t sample_at(uint32_t tick, float x, float y, float z,
                                float roll, uint8_t valid)
{
    Alarm_Sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.sample_tick_ms = tick;
    sample.acc[0] = x;
    sample.acc[1] = y;
    sample.acc[2] = z;
    sample.angle[0] = roll;
    sample.valid = valid;
    return sample;
}

static uint8_t feed(Alarm_State_t *state, Alarm_Sample_t sample)
{
    Alarm_Transition_t transition;
    return Alarm_Process(state, &sample, sample.sample_tick_ms,
                         &transition);
}

static void test_fall_requires_three_steps_and_ack(void)
{
    Alarm_State_t state;
    Alarm_Sample_t sample;
    Alarm_Transition_t transition;
    Alarm_Event_t active;
    Alarm_Event_t cleared;
    char line[128];
    uint8_t raised = 0U;
    uint32_t tick;

    Alarm_Init(&state, 0x10203040UL, 0);

    sample = sample_at(100U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "first valid sample only establishes baseline");

    /* 一次冲击只有撞击，没有姿态变化和静止确认。 */
    sample = sample_at(110U, 0.0f, 0.0f, 3.5f, 0.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "impact alone must not raise fall alarm");

    sample = sample_at(120U, 0.0f, 0.0f, 1.0f, 60.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "posture change alone is not enough before stillness");

    for (tick = 130U; tick <= 1130U; tick += 10U) {
        sample = sample_at(tick, 0.0f, 0.0f, 1.0f, 60.0f, 1U);
        transition.kind = feed(&state, sample);
        if (transition.kind == ALARM_TRANSITION_RAISE) {
            raised = 1U;
            break;
        }
    }
    CHECK(raised != 0U, "impact + posture + stillness must raise fall alarm");
    CHECK(Alarm_GetActive(&state, &active) != 0U,
          "fall event must remain active until ACK");
    CHECK(active.boot_session == 0x10203040UL && active.id == 1U &&
          active.type == ALARM_TYPE_FALL,
          "fall event carries boot, first ID and type 1");

    CHECK(Alarm_HandleAck(&state, "ALARM_ACK:2712847316:1\n", &cleared) == 0U,
          "wrong boot session ACK must be rejected");
    CHECK(Alarm_HandleAck(&state, "ALARM_ACK:270544960:2\n", &cleared) == 0U,
          "wrong event ID ACK must be rejected");
    (void)snprintf(line, sizeof(line), "ALARM_ACK:%lu:%lu\n",
                   (unsigned long)active.boot_session,
                   (unsigned long)active.id);
    CHECK(Alarm_HandleAck(&state, line, &cleared) != 0U,
          "matching ACK must clear current event");
    CHECK(cleared.boot_session == active.boot_session &&
          cleared.id == active.id && cleared.type == active.type,
          "clear result must identify the acknowledged event");
    CHECK(Alarm_HandleAck(&state, line, &cleared) == 0U,
          "expired ACK after clear must be rejected");
}

static void test_invalid_and_stale_samples_cannot_trigger(void)
{
    Alarm_State_t state;
    Alarm_Sample_t sample;

    Alarm_Init(&state, 0x55667788UL, 0);
    sample = sample_at(100U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);

    sample = sample_at(110U, 0.0f, 0.0f, 3.5f, 0.0f, 0U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "read failure cannot start a fall candidate");

    /* 旧数据即使内容像冲击，也必须被丢弃。 */
    sample = sample_at(100U, 0.0f, 0.0f, 3.5f, 60.0f, 1U);
    CHECK(Alarm_Process(&state, &sample, 300U, 0) == ALARM_TRANSITION_NONE,
          "stale sample cannot trigger an alarm");

    sample = sample_at(300U, 0.0f, 0.0f, 3.5f, 0.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "first fresh sample after invalid data resets baseline");
    CHECK(Alarm_GetActive(&state, 0) == 0U,
          "invalid/stale sequence leaves no active event");
}

static void test_shake_needs_repeated_changes(void)
{
    Alarm_State_t state;
    Alarm_Sample_t sample;
    Alarm_Transition_t transition;
    Alarm_Event_t active;
    uint32_t tick;
    uint8_t i;

    Alarm_Init(&state, 0xAABBCCDDUL, 0);
    sample = sample_at(100U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);

    /* 一个 3.5g 冲击和恢复帧，不能满足多次大变化。 */
    sample = sample_at(110U, 0.0f, 0.0f, 3.5f, 0.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "single impact must not raise shake alarm");
    sample = sample_at(120U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "single impact recovery must not raise shake alarm");

    /* 将单次冲击场景与持续抖动场景分开，避免共享同一窗口计数。 */
    Alarm_Init(&state, 0xAABBCCDDUL, 0);
    sample = sample_at(190U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);

    /* 启动后先保持静止，满足抖动检测器的重新装配条件。 */
    for (tick = 200U; tick <= 2200U; tick += 10U) {
        sample = sample_at(tick, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
        CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
              "shake warm-up must not raise an alarm");
    }

    /* 2.5g 与 0g 之间快速反复变化；每次命中间隔至少 50ms。 */
    for (i = 0U; i < 10U; i++) {
        tick = 2300U + (uint32_t)i * 60U;
        sample = sample_at(tick, (i & 1U) ? 0.0f : 3.0f,
                            0.0f, 0.0f, 0.0f, 1U);
        transition.kind = feed(&state, sample);
        if (i < 9U) {
            CHECK(transition.kind == ALARM_TRANSITION_NONE,
                  "shake threshold must require repeated changes");
        }
    }
    CHECK(transition.kind == ALARM_TRANSITION_RAISE,
          "repeated large changes in one window raise shake alarm");
    CHECK(Alarm_GetActive(&state, &active) != 0U &&
          active.type == ALARM_TYPE_SHAKE && active.id == 1U,
          "shake event uses type 2 and first event ID");

    /* 活动事件期间继续抖动，只允许重发，不允许新 ID。 */
    for (i = 0U; i < 8U; i++) {
        tick = 300U + (uint32_t)i * 10U;
        sample = sample_at(tick, (i & 1U) ? 0.0f : 1.5f,
                           0.0f, 0.0f, 0.0f, 1U);
        CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
                "active shake event must not repeat new event");
    }
    CHECK(Alarm_GetActive(&state, &active) != 0U && active.id == 1U,
          "active shake event ID remains latched");
}

static void test_shake_requires_startup_quiet_and_rearm_after_invalid(void)
{
    Alarm_State_t state;
    Alarm_Sample_t sample;
    uint32_t tick;
    uint8_t i;
    uint8_t raised;

    Alarm_Init(&state, 0x01020304UL, 0);
    sample = sample_at(0U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);

    /* 上电后立即甩动，即使连续出现大变化，也不得绕过静止预热。 */
    raised = 0U;
    for (i = 0U; i < 6U; i++) {
        tick = 10U + (uint32_t)i * 10U;
        sample = sample_at(tick, (i & 1U) ? 0.0f : 1.5f,
                           0.0f, (i & 1U) ? 0.0f : 0.0f, 0.0f, 1U);
        if (feed(&state, sample) == ALARM_TRANSITION_RAISE) {
            raised = 1U;
        }
    }
    CHECK(raised == 0U,
          "shake detector must require static warm-up after startup");

    /* 先完成一次静止预热，再注入掉线；掉线后的第一批抖动仍需重新装配。 */
    Alarm_Init(&state, 0x01020305UL, 0);
    sample = sample_at(0U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);
    for (tick = 10U; tick <= 1510U; tick += 10U) {
        sample = sample_at(tick, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
        (void)feed(&state, sample);
    }
    sample = sample_at(1520U, 0.0f, 0.0f, 1.5f, 0.0f, 0U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "invalid sample must not raise shake alarm");
    sample = sample_at(1530U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);

    raised = 0U;
    for (i = 0U; i < 6U; i++) {
        tick = 1540U + (uint32_t)i * 10U;
        sample = sample_at(tick, 0.0f, 0.0f,
                           (i & 1U) ? 1.5f : -0.5f,
                           0.0f, 1U);
        if (feed(&state, sample) == ALARM_TRANSITION_RAISE) {
            raised = 1U;
        }
    }
    CHECK(raised == 0U,
          "shake detector must rearm after an invalid sample");
}

static void test_shake_threshold_is_moderate_and_explicit(void)
{
    Alarm_Config_t config;

    Alarm_ConfigDefault(&config);
    CHECK(config.shake_delta_min_g >= 2.0f &&
          config.shake_delta_min_g <= 2.2f,
          "shake delta threshold must require a strong acceleration change");
    CHECK(config.shake_window_ms >= 1000U && config.shake_window_ms <= 1500U,
          "shake window must remain a bounded one-second-scale window");
    CHECK(config.shake_min_hits >= 10U && config.shake_min_hits <= 12U,
          "shake detector must require many repeated hits");
    CHECK(config.shake_hit_min_interval_ms >= 40U &&
          config.shake_hit_min_interval_ms <= 80U,
          "shake detector must rate-limit high-frequency duplicate hits");
}

static void test_slow_motion_does_not_trigger_shake(void)
{
    Alarm_State_t state;
    Alarm_Sample_t sample;
    Alarm_Event_t active;
    uint32_t tick;
    uint8_t i;

    Alarm_Init(&state, 0x0A0B0C0DUL, 0);
    sample = sample_at(0U, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);

    /* 先完成启动静止重装配。 */
    for (tick = 10U; tick <= 1510U; tick += 10U) {
        sample = sample_at(tick, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
        CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
              "slow-motion warm-up must not raise an alarm");
    }

    /* 普通缓慢移动：每次 ACC 变化远低于抖动阈值，不应累计命中。 */
    for (i = 1U; i <= 12U; i++) {
        tick = 1510U + (uint32_t)i * 100U;
        sample = sample_at(tick, (float)i * 0.15f, 0.0f, 1.0f,
                           0.0f, 1U);
        CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
              "ordinary slow movement must not raise a shake alarm");
    }
    CHECK(Alarm_GetActive(&state, &active) == 0U,
          "ordinary slow movement leaves no active alarm");
}

static void test_protocol_format_and_ack_parser(void)
{
    Alarm_Event_t event;
    Alarm_State_t state;
    char line[128];
    uint32_t boot;
    uint32_t id;

    event.boot_session = 305419896UL;
    event.id = 7U;
    event.type = ALARM_TYPE_SHAKE;
    CHECK(Alarm_FormatEvent(line, sizeof(line), &event, 1U) > 0,
          "active alarm frame formats");
    CHECK(strcmp(line,
                 "ALARM|BOOT=305419896|ID=7|TYPE=2|ACTIVE=1\r\n") == 0,
          "active alarm frame matches monitor protocol");
    CHECK(Alarm_FormatEvent(line, sizeof(line), &event, 0U) > 0 &&
          strstr(line, "ACTIVE=0\r\n") != 0,
          "clear alarm frame carries ACTIVE=0");
    Alarm_Init(&state, event.boot_session, 0);
    CHECK(Alarm_FormatState(line, sizeof(line), &state) > 0 &&
          strcmp(line,
                 "ALARM_STATE|BOOT=305419896|ACTIVE=0|ID=0|TYPE=0\r\n") == 0,
          "inactive alarm state heartbeat formats");
    state.active = 1U;
    state.active_event = event;
    CHECK(Alarm_FormatState(line, sizeof(line), &state) > 0 &&
          strcmp(line,
                 "ALARM_STATE|BOOT=305419896|ACTIVE=1|ID=7|TYPE=2\r\n") == 0,
          "active alarm state heartbeat formats");
    CHECK(Alarm_ParseAck("ALARM_ACK:305419896:7\n", &boot, &id) != 0U &&
          boot == event.boot_session && id == event.id,
          "new boot-aware ACK parses");
    CHECK(Alarm_ParseAck("ALARM_ACK:305419896:7\r\n", &boot, &id) != 0U,
          "CRLF ACK parses");
    CHECK(Alarm_ParseAck("ALARM_ACK:305419896:7x\n", &boot, &id) == 0U,
          "ACK with trailing data is rejected");
    CHECK(Alarm_ParseAck("ALARM_ACK:4294967296:7\n", &boot, &id) == 0U,
          "overflowing boot session is rejected");
}

static void test_tick_wrap_and_roll_wrap(void)
{
    Alarm_State_t state;
    Alarm_State_t stale_state;
    Alarm_Sample_t sample;
    uint32_t tick;
    uint32_t i;
    uint8_t raised = 0U;

    /* 样本时间和当前时间跨过 uint32_t 回绕后，过旧数据仍应拒绝。 */
    Alarm_Init(&stale_state, 0x01020304UL, 0);
    sample = sample_at(0xFFFFFF00UL, 0.0f, 0.0f, 3.5f, 0.0f, 1U);
    CHECK(Alarm_Process(&stale_state, &sample, 0x00000020UL, 0) ==
              ALARM_TRANSITION_NONE && stale_state.previous_valid == 0U,
          "sample older than freshness window is rejected across wrap");

    /* 之后的 48ms 样本应恢复为有效基线。 */
    sample = sample_at(0xFFFFFFF0UL, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    CHECK(Alarm_Process(&stale_state, &sample, 0x00000020UL, 0) ==
              ALARM_TRANSITION_NONE && stale_state.previous_valid != 0U,
          "fresh sample age remains valid across uint32 tick wrap");

    /* 跨回绕完成“冲击 + 姿态变化”，并让静止确认计时器也跨回绕。 */
    Alarm_Init(&state, 0x11223344UL, 0);
    sample = sample_at(0xFFFFFF00UL, 0.0f, 0.0f, 1.0f, 0.0f, 1U);
    (void)feed(&state, sample);
    sample = sample_at(0xFFFFFF10UL, 0.0f, 0.0f, 3.5f, 0.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "fall impact can occur before tick wrap");
    sample = sample_at(0xFFFFFF20UL, 0.0f, 0.0f, 1.0f, 60.0f, 1U);
    CHECK(feed(&state, sample) == ALARM_TRANSITION_NONE,
          "fall posture change can occur before tick wrap");

    for (i = 0U; i <= 100U; i++) {
        tick = 0xFFFFFF30UL + i * 10U;
        sample = sample_at(tick, 0.0f, 0.0f, 1.0f, 60.0f, 1U);
        if (feed(&state, sample) == ALARM_TRANSITION_RAISE) {
            raised = 1U;
            break;
        }
    }
    CHECK(raised != 0U,
          "stillness confirmation timer remains correct across tick wrap");

    /* 179 -> -179 只有 2 度，不得被当成 358 度姿态跳变而误报。 */
    Alarm_Init(&state, 0x55667799UL, 0);
    sample = sample_at(0xFFFFFF00UL, 0.0f, 0.0f, 1.0f, 179.0f, 1U);
    (void)feed(&state, sample);
    sample = sample_at(0xFFFFFF10UL, 0.0f, 0.0f, 3.5f, 179.0f, 1U);
    (void)feed(&state, sample);
    sample = sample_at(0xFFFFFF20UL, 0.0f, 0.0f, 1.0f, -179.0f, 1U);
    (void)feed(&state, sample);
    raised = 0U;
    for (i = 0U; i < 120U; i++) {
        tick = 0xFFFFFF30UL + i * 10U;
        sample = sample_at(tick, 0.0f, 0.0f, 1.0f, -179.0f, 1U);
        if (feed(&state, sample) == ALARM_TRANSITION_RAISE) {
            raised = 1U;
            break;
        }
    }
    CHECK(raised == 0U,
          "roll boundary crossing must not create a false posture change");
}

int main(void)
{
    test_fall_requires_three_steps_and_ack();
    test_invalid_and_stale_samples_cannot_trigger();
    test_shake_needs_repeated_changes();
    test_shake_requires_startup_quiet_and_rearm_after_invalid();
    test_shake_threshold_is_moderate_and_explicit();
    test_slow_motion_does_not_trigger_shake();
    test_protocol_format_and_ack_parser();
    test_tick_wrap_and_roll_wrap();

    if (failures != 0) {
        printf("alarm_engine_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("alarm_engine_test: all checks passed\n");
    return 0;
}
