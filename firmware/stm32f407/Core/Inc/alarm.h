#ifndef __ALARM_H
#define __ALARM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 对外协议中的报警类型，保持 1/2 编号不变。 */
#define ALARM_TYPE_FALL   1U
#define ALARM_TYPE_SHAKE  2U

#define ALARM_TRANSITION_NONE   0U
#define ALARM_TRANSITION_RAISE  1U

typedef struct {
    /* 传感器时间戳超过该值，样本不得进入报警判定。 */
    uint32_t max_sensor_age_ms;

    /* 疑似跌倒：冲击、姿态变化、静止确认。单位分别为 g 和度。 */
    float impact_min_g;
    float posture_change_min_deg;
    uint32_t posture_window_ms;
    float still_acc_min_g;
    float still_acc_max_g;
    float still_delta_max_g;
    float still_angle_step_max_deg;
    uint32_t still_confirm_ms;

    /* 异常剧烈抖动：一个窗口内必须出现多次大变化。 */
    float shake_delta_min_g;
    uint32_t shake_window_ms;
    uint16_t shake_min_hits;
    uint32_t shake_hit_min_interval_ms;

    /* 事件触发后，恢复一段安静时间才允许该检测器再次触发。 */
    uint32_t rearm_quiet_ms;
} Alarm_Config_t;

typedef struct {
    float acc[3];
    float angle[3];
    uint8_t valid;
    uint32_t sample_tick_ms;
} Alarm_Sample_t;

typedef struct {
    uint32_t boot_session;
    uint32_t id;
    uint8_t type;
} Alarm_Event_t;

typedef struct {
    uint8_t kind;
    Alarm_Event_t event;
} Alarm_Transition_t;

typedef struct {
    Alarm_Config_t config;
    uint32_t boot_session;
    uint32_t next_event_id;

    uint8_t active;
    Alarm_Event_t active_event;

    /* 0=待机，1=撞击后等待姿态变化，2=等待静止，3=冷却重启。 */
    uint8_t fall_state;
    uint32_t fall_impact_ms;
    float fall_pre_roll;
    float fall_pre_pitch;
    uint32_t fall_still_start_ms;
    uint8_t fall_still_started;
    uint32_t fall_quiet_start_ms;
    uint8_t fall_quiet_started;

    uint8_t shake_rearmed;
    uint8_t shake_window_active;
    uint16_t shake_hits;
    uint32_t shake_window_start_ms;
    uint32_t shake_last_hit_ms;
    uint8_t shake_hit_seen;
    uint32_t shake_quiet_start_ms;
    uint8_t shake_quiet_started;

    Alarm_Sample_t previous;
    uint8_t previous_valid;
} Alarm_State_t;

void Alarm_ConfigDefault(Alarm_Config_t *config);
void Alarm_Init(Alarm_State_t *state, uint32_t boot_session,
                const Alarm_Config_t *config);

/* 单次调用只处理一个样本，不延时、不轮询外设。返回 transition->kind。 */
uint8_t Alarm_Process(Alarm_State_t *state, const Alarm_Sample_t *sample,
                      uint32_t now_ms, Alarm_Transition_t *transition);

/* 仅接受 ALARM_ACK:<boot>:<id>，并且必须匹配当前活动事件。 */
uint8_t Alarm_ParseAck(const char *line, uint32_t *boot_session,
                       uint32_t *event_id);
uint8_t Alarm_HandleAck(Alarm_State_t *state, const char *line,
                        Alarm_Event_t *cleared_event);

uint8_t Alarm_GetActive(const Alarm_State_t *state, Alarm_Event_t *event);
uint32_t Alarm_GetBootSession(const Alarm_State_t *state);

/* 返回写入的字节数；缓冲区不足或参数非法时返回 0。 */
int Alarm_FormatEvent(char *out, uint16_t out_len,
                      const Alarm_Event_t *event, uint8_t active);
int Alarm_FormatState(char *out, uint16_t out_len,
                      const Alarm_State_t *state);

#ifdef __cplusplus
}
#endif

#endif /* __ALARM_H */
