#include "alarm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    ALARM_FALL_IDLE = 0U,
    ALARM_FALL_IMPACT_WAIT = 1U,
    ALARM_FALL_STILL_WAIT = 2U,
    ALARM_FALL_COOLDOWN = 3U
};

static float alarm_acc_magnitude(const float acc[3])
{
    return sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
}

static float alarm_acc_delta(const float a[3], const float b[3])
{
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float alarm_short_angle_delta(float current, float reference)
{
    float delta = current - reference;

    /* JY61P 的角度在 -180..180 间表示；跨边界时取最短有符号差。 */
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    return delta;
}

static float alarm_posture_delta(const Alarm_State_t *state,
                                 const Alarm_Sample_t *sample)
{
    float dr = alarm_short_angle_delta(sample->angle[0],
                                       state->fall_pre_roll);
    float dp = alarm_short_angle_delta(sample->angle[1],
                                       state->fall_pre_pitch);
    return sqrtf(dr * dr + dp * dp);
}

static float alarm_angle_step(const Alarm_Sample_t *a,
                              const Alarm_Sample_t *b)
{
    float dr = alarm_short_angle_delta(a->angle[0], b->angle[0]);
    float dp = alarm_short_angle_delta(a->angle[1], b->angle[1]);
    return sqrtf(dr * dr + dp * dp);
}

static uint8_t alarm_float_is_reasonable(float value)
{
    /* 同时拒绝 NaN 和无穷大，避免异常浮点值推进状态机。 */
    if (value != value) return 0U;
    if (value > 10000.0f || value < -10000.0f) return 0U;
    return 1U;
}

static uint8_t alarm_sample_is_fresh(const Alarm_State_t *state,
                                     const Alarm_Sample_t *sample,
                                     uint32_t now_ms)
{
    uint32_t age;
    uint8_t i;

    if (sample->valid == 0U) return 0U;

    age = now_ms - sample->sample_tick_ms;
    if (age > 0x7FFFFFFFUL || age > state->config.max_sensor_age_ms) {
        return 0U;
    }

    if (state->previous_valid != 0U) {
        /* 相同或倒退的时间戳表示重复/旧样本，不能重复计数。 */
        uint32_t step = sample->sample_tick_ms -
                         state->previous.sample_tick_ms;
        if (step == 0U || step > 0x7FFFFFFFUL) {
            return 0U;
        }
    }

    for (i = 0U; i < 3U; i++) {
        if (alarm_float_is_reasonable(sample->acc[i]) == 0U ||
            alarm_float_is_reasonable(sample->angle[i]) == 0U) {
            return 0U;
        }
    }
    return 1U;
}

static void alarm_transition_reset(Alarm_Transition_t *transition)
{
    transition->kind = ALARM_TRANSITION_NONE;
    transition->event.boot_session = 0U;
    transition->event.id = 0U;
    transition->event.type = 0U;
}

static void alarm_reset_invalid_sample(Alarm_State_t *state)
{
    state->previous_valid = 0U;

    /* 已经触发过的检测器保留冷却状态，防止断线后立即重复报警。 */
    if (state->fall_state != ALARM_FALL_COOLDOWN) {
        state->fall_state = ALARM_FALL_IDLE;
    }
    state->fall_still_started = 0U;
    state->fall_quiet_started = 0U;

    state->shake_window_active = 0U;
    state->shake_hits = 0U;
    state->shake_hit_seen = 0U;
    /* 重新收到有效数据后仍需重新完成静止装配。 */
    state->shake_rearmed = 0U;
    state->shake_quiet_started = 0U;
}

static uint8_t alarm_raise(Alarm_State_t *state, uint8_t type,
                           Alarm_Transition_t *transition)
{
    if (state->active != 0U) {
        return 0U;
    }

    state->active_event.boot_session = state->boot_session;
    state->active_event.id = state->next_event_id;
    state->active_event.type = type;
    state->active = 1U;

    state->next_event_id++;
    if (state->next_event_id == 0U) {
        state->next_event_id = 1U;
    }

    transition->kind = ALARM_TRANSITION_RAISE;
    transition->event = state->active_event;
    return 1U;
}

static uint8_t alarm_is_still(const Alarm_State_t *state,
                              const Alarm_Sample_t *sample,
                              float acc_mag, float acc_delta,
                              float angle_step)
{
    if (acc_mag < state->config.still_acc_min_g ||
        acc_mag > state->config.still_acc_max_g) {
        return 0U;
    }
    if (acc_delta > state->config.still_delta_max_g) return 0U;
    if (angle_step > state->config.still_angle_step_max_deg) return 0U;
    (void)sample;
    return 1U;
}

static uint8_t alarm_step_fall(Alarm_State_t *state,
                               const Alarm_Sample_t *sample,
                               uint32_t now_ms, float acc_mag,
                               float acc_delta, float angle_step,
                               Alarm_Transition_t *transition)
{
    uint8_t still = alarm_is_still(state, sample, acc_mag,
                                   acc_delta, angle_step);
    float posture_delta;

    switch (state->fall_state) {
    case ALARM_FALL_IDLE:
        if (acc_mag >= state->config.impact_min_g &&
            state->previous_valid != 0U) {
            /* 姿态基准取冲击前一帧，而不是取可能已经翻转的当前帧。 */
            state->fall_pre_roll = state->previous.angle[0];
            state->fall_pre_pitch = state->previous.angle[1];
            state->fall_impact_ms = now_ms;
            state->fall_still_started = 0U;
            state->fall_state = ALARM_FALL_IMPACT_WAIT;
        }
        break;

    case ALARM_FALL_IMPACT_WAIT:
        if ((uint32_t)(now_ms - state->fall_impact_ms) >
            state->config.posture_window_ms) {
            state->fall_state = ALARM_FALL_IDLE;
            state->fall_still_started = 0U;
            break;
        }

        posture_delta = alarm_posture_delta(state, sample);
        if (posture_delta >= state->config.posture_change_min_deg) {
            state->fall_state = ALARM_FALL_STILL_WAIT;
            if (still != 0U) {
                state->fall_still_start_ms = now_ms;
                state->fall_still_started = 1U;
            } else {
                state->fall_still_started = 0U;
            }
        }
        break;

    case ALARM_FALL_STILL_WAIT:
        if ((uint32_t)(now_ms - state->fall_impact_ms) >
            state->config.posture_window_ms) {
            state->fall_state = ALARM_FALL_IDLE;
            state->fall_still_started = 0U;
            break;
        }

        posture_delta = alarm_posture_delta(state, sample);
        if (posture_delta < state->config.posture_change_min_deg) {
            state->fall_state = ALARM_FALL_IMPACT_WAIT;
            state->fall_still_started = 0U;
            break;
        }

        if (still == 0U) {
            state->fall_still_started = 0U;
        } else if (state->fall_still_started == 0U) {
            state->fall_still_start_ms = now_ms;
            state->fall_still_started = 1U;
        } else if ((uint32_t)(now_ms - state->fall_still_start_ms) >=
                   state->config.still_confirm_ms) {
            state->fall_state = ALARM_FALL_COOLDOWN;
            state->fall_still_started = 0U;
            state->fall_quiet_started = 0U;
            (void)alarm_raise(state, ALARM_TYPE_FALL, transition);
        }
        break;

    case ALARM_FALL_COOLDOWN:
        if (still == 0U) {
            state->fall_quiet_started = 0U;
        } else if (state->fall_quiet_started == 0U) {
            state->fall_quiet_start_ms = now_ms;
            state->fall_quiet_started = 1U;
        } else if ((uint32_t)(now_ms - state->fall_quiet_start_ms) >=
                   state->config.rearm_quiet_ms) {
            state->fall_state = ALARM_FALL_IDLE;
            state->fall_quiet_started = 0U;
        }
        break;

    default:
        state->fall_state = ALARM_FALL_IDLE;
        state->fall_still_started = 0U;
        state->fall_quiet_started = 0U;
        break;
    }

    return transition->kind;
}

static uint8_t alarm_step_shake(Alarm_State_t *state, uint32_t now_ms,
                                 float acc_mag, float acc_delta,
                                 Alarm_Transition_t *transition)
{
    /* 启动/掉线后的重装配必须是真正安静，而不是“变化小于报警阈值”。
       否则缓慢移动也可能在预热期间把检测器提前置为可用。 */
    uint8_t quiet_sample = (acc_delta <= state->config.still_delta_max_g) &&
                           (acc_mag >= state->config.still_acc_min_g) &&
                           (acc_mag <= state->config.still_acc_max_g);

    if (state->shake_rearmed == 0U) {
        if (quiet_sample == 0U) {
            state->shake_quiet_started = 0U;
        } else if (state->shake_quiet_started == 0U) {
            state->shake_quiet_start_ms = now_ms;
            state->shake_quiet_started = 1U;
        } else if ((uint32_t)(now_ms - state->shake_quiet_start_ms) >=
                   state->config.rearm_quiet_ms) {
            state->shake_rearmed = 1U;
            state->shake_quiet_started = 0U;
        }
        state->shake_window_active = 0U;
        state->shake_hits = 0U;
        return transition->kind;
    }

    if (acc_delta < state->config.shake_delta_min_g) {
        /* 普通帧不计数，但仍允许后续大变化落在同一个窗口中。 */
        return transition->kind;
    }

    /* 同一次陡变在高频轮询中只能算一次，避免普通甩手被密集采样放大。 */
    if (state->shake_hit_seen != 0U &&
        (uint32_t)(now_ms - state->shake_last_hit_ms) <
        state->config.shake_hit_min_interval_ms) {
        return transition->kind;
    }

    state->shake_quiet_started = 0U;
    state->shake_last_hit_ms = now_ms;
    state->shake_hit_seen = 1U;
    if (state->shake_window_active == 0U ||
        (uint32_t)(now_ms - state->shake_window_start_ms) >
        state->config.shake_window_ms) {
        state->shake_window_start_ms = now_ms;
        state->shake_hits = 1U;
        state->shake_window_active = 1U;
    } else if (state->shake_hits < 0xFFFFU) {
        state->shake_hits++;
    }

    if (state->shake_hits >= state->config.shake_min_hits) {
        state->shake_rearmed = 0U;
        state->shake_window_active = 0U;
        state->shake_hits = 0U;
        state->shake_hit_seen = 0U;
        (void)alarm_raise(state, ALARM_TYPE_SHAKE, transition);
    }
    return transition->kind;
}

void Alarm_ConfigDefault(Alarm_Config_t *config)
{
    if (config == 0) return;

    config->max_sensor_age_ms = 120U;
    config->impact_min_g = 2.5f;
    config->posture_change_min_deg = 45.0f;
    config->posture_window_ms = 1500U;
    config->still_acc_min_g = 0.75f;
    config->still_acc_max_g = 1.25f;
    config->still_delta_max_g = 0.20f;
    config->still_angle_step_max_deg = 8.0f;
    config->still_confirm_ms = 1000U;
    config->shake_delta_min_g = 2.0f;
    config->shake_window_ms = 1200U;
    config->shake_min_hits = 10U;
    config->shake_hit_min_interval_ms = 50U;
    config->rearm_quiet_ms = 2000U;
}

void Alarm_Init(Alarm_State_t *state, uint32_t boot_session,
                const Alarm_Config_t *config)
{
    if (state == 0) return;

    memset(state, 0, sizeof(*state));
    if (config != 0) {
        state->config = *config;
    } else {
        Alarm_ConfigDefault(&state->config);
    }

    state->boot_session = (boot_session == 0U) ? 1U : boot_session;
    state->next_event_id = 1U;
    state->fall_state = ALARM_FALL_IDLE;
    /* 上电后先完成一段静止重装配，首批传感器跳变不参与报警。 */
    state->shake_rearmed = 0U;
}

uint8_t Alarm_Process(Alarm_State_t *state, const Alarm_Sample_t *sample,
                      uint32_t now_ms, Alarm_Transition_t *transition)
{
    Alarm_Transition_t local_transition;
    float acc_mag;
    float acc_delta;
    float angle_step;

    if (transition == 0) {
        transition = &local_transition;
    }
    alarm_transition_reset(transition);

    if (state == 0 || sample == 0) return ALARM_TRANSITION_NONE;

    if (alarm_sample_is_fresh(state, sample, now_ms) == 0U) {
        alarm_reset_invalid_sample(state);
        return ALARM_TRANSITION_NONE;
    }

    if (state->previous_valid == 0U) {
        state->previous = *sample;
        state->previous_valid = 1U;
        return ALARM_TRANSITION_NONE;
    }

    acc_mag = alarm_acc_magnitude(sample->acc);
    acc_delta = alarm_acc_delta(sample->acc, state->previous.acc);
    angle_step = alarm_angle_step(sample, &state->previous);

    /* 两个检测器都更新自己的重启状态；活动事件只禁止新事件，不冻结采样。 */
    (void)alarm_step_fall(state, sample, now_ms, acc_mag, acc_delta,
                          angle_step, transition);
    (void)alarm_step_shake(state, now_ms, acc_mag, acc_delta, transition);

    state->previous = *sample;
    state->previous_valid = 1U;
    return transition->kind;
}

static uint8_t alarm_parse_u32(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    uint32_t result = 0U;
    uint8_t digits = 0U;

    while (*p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p - '0');
        if (result > (0xFFFFFFFFUL - digit) / 10UL) {
            return 0U;
        }
        result = result * 10UL + digit;
        p++;
        digits = 1U;
    }
    if (digits == 0U) return 0U;

    *cursor = p;
    *value = result;
    return 1U;
}

uint8_t Alarm_ParseAck(const char *line, uint32_t *boot_session,
                       uint32_t *event_id)
{
    const char *p;
    uint32_t parsed_boot;
    uint32_t parsed_id;

    if (line == 0 || boot_session == 0 || event_id == 0) return 0U;
    if (strncmp(line, "ALARM_ACK:", 10U) != 0) return 0U;

    p = line + 10;
    if (alarm_parse_u32(&p, &parsed_boot) == 0U) return 0U;
    if (*p != ':') return 0U;
    p++;
    if (alarm_parse_u32(&p, &parsed_id) == 0U) return 0U;

    while (*p == '\r' || *p == '\n') p++;
    if (*p != '\0') return 0U;

    *boot_session = parsed_boot;
    *event_id = parsed_id;
    return 1U;
}

uint8_t Alarm_HandleAck(Alarm_State_t *state, const char *line,
                        Alarm_Event_t *cleared_event)
{
    uint32_t boot_session;
    uint32_t event_id;

    if (state == 0 ||
        Alarm_ParseAck(line, &boot_session, &event_id) == 0U) {
        return 0U;
    }
    if (state->active == 0U ||
        boot_session != state->active_event.boot_session ||
        event_id != state->active_event.id) {
        return 0U;
    }

    if (cleared_event != 0) {
        *cleared_event = state->active_event;
    }
    state->active = 0U;
    return 1U;
}

uint8_t Alarm_GetActive(const Alarm_State_t *state, Alarm_Event_t *event)
{
    if (state == 0 || event == 0 || state->active == 0U) return 0U;
    *event = state->active_event;
    return 1U;
}

uint32_t Alarm_GetBootSession(const Alarm_State_t *state)
{
    return (state == 0) ? 0U : state->boot_session;
}

int Alarm_FormatEvent(char *out, uint16_t out_len,
                      const Alarm_Event_t *event, uint8_t active)
{
    int len;

    if (out == 0 || out_len == 0U || event == 0) return 0;
    len = snprintf(out, out_len,
                   "ALARM|BOOT=%lu|ID=%lu|TYPE=%u|ACTIVE=%u\r\n",
                   (unsigned long)event->boot_session,
                   (unsigned long)event->id,
                   (unsigned)event->type,
                   (unsigned)((active != 0U) ? 1U : 0U));
    if (len <= 0 || len >= (int)out_len) return 0;
    return len;
}

int Alarm_FormatState(char *out, uint16_t out_len,
                      const Alarm_State_t *state)
{
    uint8_t active;
    uint32_t id;
    uint8_t type;
    int len;

    if (out == 0 || out_len == 0U || state == 0) return 0;
    active = (state->active != 0U) ? 1U : 0U;
    id = active != 0U ? state->active_event.id : 0U;
    type = active != 0U ? state->active_event.type : 0U;
    len = snprintf(out, out_len,
                   "ALARM_STATE|BOOT=%lu|ACTIVE=%u|ID=%lu|TYPE=%u\r\n",
                   (unsigned long)state->boot_session,
                   (unsigned)active,
                   (unsigned long)id,
                   (unsigned)type);
    if (len <= 0 || len >= (int)out_len) return 0;
    return len;
}
