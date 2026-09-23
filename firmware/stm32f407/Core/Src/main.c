/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 双手手语翻译手套 - 主调度框架（防御性重构 v3.1）
  * @author         : 项目维护重构版 v3.1
  * @date           : 2026-06-29
  * @note 多速率时序调度（基于 TIM3 1ms 系统 tick）
 *   10ms  / 100Hz : 右手 JY61P Read
  *   10ms  / 100Hz : 跌倒检测 + MAX30102 FIFO 分时读取
  *   20ms  /  50Hz : ADC Ping-Pong 快照 + Flex_Update + 痉挛检测
 *   50ms  /  20Hz : 手势识别 + 振动维护
  *   100ms /  10Hz : 蓝牙指令消费 + SOS 告警 + MAX30102 批处理
 *   250ms /   4Hz : FLEX/IMU 快速遥测；诊断帧每秒一次
  * @note 防御性设计
  *   1. Flex 采样链路使用 Ping-Pong DMA，避免读取时被后台 DMA 覆盖。
  *   2. MAX30102 以 10ms 轮询对齐 100Hz，降低 FIFO 失步和溢出风险。
  *   3. 心率/血氧越界判定下沉到驱动层，main 仅消费最终结果。
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

#include "adc.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "i2c.h"
#include "flex_sensor.h"
#include "jy61p.h"
#include "dfplayer.h"
#include "bluetooth.h"
#include "vibrator.h"
#include "gesture.h"
#include "max30102.h"
#include "bringup_diag.h"
#include "soft_uart.h"
#include "alarm.h"
#include "alarm_session.h"
#include "math.h"
#include "stdio.h"
#include "string.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/**
 * v3.1 升级说明：柔性传感器采样改为 ADC Ping-Pong 双缓冲
 * （HalfCplt/Cplt 中断），细节见 flex_sensor.c 与 adc.c 的 USER CODE。
 * 旧版 ADC_DMA_PAUSE/RESUME 宏已废弃。
 */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* MAX30102 仅作为展示遥测；外部蜂鸣器只由 JY61P 固件安全事件驱动。 */

/*
 * Temporary MAX30102 bring-up mode. At 9600 baud the five verbose telemetry
 * strings occupy most of the telemetry bandwidth. Keep only compact
 * PPG diagnostics while validating the sensor; set to 0U after bring-up.
 */
#define MAX30102_DIAG_ONLY 0U
#define BLUETOOTH_FAST_TELEMETRY_PERIOD_MS 250U
#define BLUETOOTH_DIAG_PERIOD_MS          1000U
#define BRINGUP_DIAG_PERIOD_MS            5000U
#define JY_RECOVERY_PERIOD_MS          500U

/* 当前硬件配置：启用右手 JY61P，关闭左手 JY61P，关闭 DFPlayer，启用蜂鸣器。 */
#define JY61P_RIGHT_ENABLE 1U
#define JY61P_LEFT_ENABLE  0U
#define JY61P_ENABLE       (JY61P_RIGHT_ENABLE || JY61P_LEFT_ENABLE)
#define DFPLAYER_ENABLE    0U
#define BUZZER_ENABLE      1U

/* 报警演示参数：只用于样机调参，不是医疗验证阈值。 */
#define ALARM_MAX_SENSOR_AGE_MS          120U
#define ALARM_FALL_IMPACT_MIN_G          2.5f
#define ALARM_FALL_POSTURE_MIN_DEG       45.0f
#define ALARM_FALL_POSTURE_WINDOW_MS     1500U
#define ALARM_FALL_STILL_MIN_G           0.75f
#define ALARM_FALL_STILL_MAX_G           1.25f
#define ALARM_FALL_STILL_DELTA_MAX_G     0.20f
#define ALARM_FALL_STILL_ANGLE_MAX_DEG   8.0f
#define ALARM_FALL_STILL_CONFIRM_MS      1000U
#define ALARM_SHAKE_DELTA_MIN_G          2.0f
#define ALARM_SHAKE_WINDOW_MS            1200U
#define ALARM_SHAKE_MIN_HITS             10U
#define ALARM_SHAKE_HIT_MIN_INTERVAL_MS    50U
#define ALARM_REARM_QUIET_MS             2000U

/* 活动事件仍由协议周期重发；本地蜂鸣器到时自动静音，避免长鸣。 */
#define ALARM_REPEAT_PERIOD_MS            1000U
#define ALARM_LOCAL_BUZZER_TIMEOUT_MS    30000U
#define BUZZER_PWM_COMPARE                500U
#define BUZZER_PWM_SILENT_COMPARE        1000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* ADC DMA 循环缓冲（每只手 5 通道，Ping-Pong 双半区） */
volatile uint16_t adc1_buf[ADC_PINGPONG_SIZE];  /* 左手 5ch x 2 half-buffer */
volatile uint16_t adc2_buf[ADC_PINGPONG_SIZE];  /* 右手 5ch x 2 half-buffer */

/* 多速率调度时间戳 */
static uint32_t t_jy_poll = 0;
static uint32_t t_10ms  = 0;
static uint32_t t_ppg_fifo = 0;
static uint32_t t_20ms  = 0;
static uint32_t t_50ms  = 0;
static uint32_t t_100ms = 0;
static uint32_t t_200ms = 0;
static uint32_t t_bt_diag = 0;
static uint32_t t_bringup_diag = 0;

/* 全局系统 tick，由 TIM3 1ms 中断递增 */
volatile uint32_t sys_tick_ms = 0;

/* UART3 中断接收单字节缓冲 */
static volatile uint8_t uart3_rx_byte;

/* MAX30102 初始化状态 */
static uint8_t max30102_present = 0;  /* 1 = 设备在线且初始化成功 */

/* 报警算法状态与本次启动会话号，均只存在 RAM。 */
static Alarm_State_t alarm_state;
static uint32_t alarm_boot_session = 0U;
static uint32_t alarm_last_tx_ms = 0U;
static uint8_t  alarm_tx_seen = 0U;
static Alarm_Event_t alarm_last_cleared_event;
static uint8_t  alarm_last_cleared_valid = 0U;
static uint8_t  alarm_clear_pending = 0U;
static uint32_t alarm_buzzer_started_ms = 0U;
static uint8_t  alarm_buzzer_active = 0U;
static uint32_t t_jy_right_recover = 0U;

/* JY61P 双手姿态数据（由驱动层维护） */
extern JY61P_Data_t JY61P_Right;
extern JY61P_Data_t JY61P_Left;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static uint8_t Alarm_MainTimestampFresh(uint32_t now_ms, uint32_t stamp_ms)
{
    uint32_t age = now_ms - stamp_ms;
    return (age <= 0x7FFFFFFFUL && age <= ALARM_MAX_SENSOR_AGE_MS) ? 1U : 0U;
}

static uint32_t JY_MainAccAge(uint32_t now_ms, const JY61P_Data_t *data)
{
    uint32_t age;

    if (data == 0 || data->acc_sample_seen == 0U) {
        return 0xFFFFFFFFUL;
    }

    age = now_ms - data->acc_updated_ms;
    if (age > 0x7FFFFFFFUL) {
        return 0xFFFFFFFFUL;
    }
    return age;
}

static uint8_t Alarm_MainSendEvent(const Alarm_Event_t *event, uint8_t active)
{
    char line[96];

    if (Alarm_FormatEvent(line, sizeof(line), event, active) > 0) {
        return BT_SendAlarmString(line);
    }
    return 0U;
}

static void Alarm_MainRetryClear(void)
{
    if (alarm_clear_pending != 0U && alarm_last_cleared_valid != 0U) {
        if (Alarm_MainSendEvent(&alarm_last_cleared_event, 0U) != 0U) {
            alarm_clear_pending = 0U;
        }
    }
}

static void Alarm_MainStartBuzzer(uint32_t now_ms)
{
#if BUZZER_ENABLE
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, BUZZER_PWM_COMPARE);
    alarm_buzzer_started_ms = now_ms;
    alarm_buzzer_active = 1U;
#else
    (void)now_ms;
#endif
}

static void Alarm_MainStopBuzzer(void)
{
#if BUZZER_ENABLE
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, BUZZER_PWM_SILENT_COMPARE);
#endif
    alarm_buzzer_active = 0U;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  uint8_t jy_right_ret;
#if JY61P_LEFT_ENABLE
  uint8_t jy_left_ret;
#endif
  Alarm_Config_t alarm_config;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */
  SystemClock_Config();

  /* 启动会话号只写 RAM：优先硬件 RNG，不依赖 Flash 计数器。 */
  alarm_boot_session = AlarmSession_Generate();
  Alarm_ConfigDefault(&alarm_config);
  alarm_config.max_sensor_age_ms = ALARM_MAX_SENSOR_AGE_MS;
  alarm_config.impact_min_g = ALARM_FALL_IMPACT_MIN_G;
  alarm_config.posture_change_min_deg = ALARM_FALL_POSTURE_MIN_DEG;
  alarm_config.posture_window_ms = ALARM_FALL_POSTURE_WINDOW_MS;
  alarm_config.still_acc_min_g = ALARM_FALL_STILL_MIN_G;
  alarm_config.still_acc_max_g = ALARM_FALL_STILL_MAX_G;
  alarm_config.still_delta_max_g = ALARM_FALL_STILL_DELTA_MAX_G;
  alarm_config.still_angle_step_max_deg = ALARM_FALL_STILL_ANGLE_MAX_DEG;
  alarm_config.still_confirm_ms = ALARM_FALL_STILL_CONFIRM_MS;
  alarm_config.shake_delta_min_g = ALARM_SHAKE_DELTA_MIN_G;
  alarm_config.shake_window_ms = ALARM_SHAKE_WINDOW_MS;
  alarm_config.shake_min_hits = ALARM_SHAKE_MIN_HITS;
  alarm_config.shake_hit_min_interval_ms = ALARM_SHAKE_HIT_MIN_INTERVAL_MS;
  alarm_config.rearm_quiet_ms = ALARM_REARM_QUIET_MS;
  Alarm_Init(&alarm_state, alarm_boot_session, &alarm_config);

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
#if JY61P_ENABLE
#if JY61P_RIGHT_ENABLE
  MX_I2C1_Init();
#endif
#if JY61P_LEFT_ENABLE
  MX_I2C2_Init();
#endif
#endif
  MX_I2C3_Init();
#if DFPLAYER_ENABLE
  MX_USART1_UART_Init();
#endif
  MX_USART3_UART_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_TIM3_Init();
  MX_TIM4_Init();

  /* 启动 TIM3 1ms 中断，作为统一调度时钟源 */
  HAL_TIM_Base_Start_IT(&htim3);

  /* USER CODE BEGIN 2 */
  BringupDiag_Init();

  /* 阶段 1：基础通信与采样链路初始化 */
  Flex_Init();         /* 柔性传感器极值/方向初始化 */

  /* 阶段 2：启动 ADC DMA 循环扫描（双 ADC 独立 DMA） */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc1_buf, ADC_PINGPONG_SIZE);
  HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adc2_buf, ADC_PINGPONG_SIZE);

  /* 阶段 3：初始化已启用的 JY61P 通道 */
#if JY61P_RIGHT_ENABLE
  jy_right_ret = JY61P_Init(JY61P_CH_RIGHT);
  if (jy_right_ret != 0U) {
      BringupDiag_SetJYRightResult(0, jy_right_ret);
      /* 右手 JY61P 未就绪：检查 3.3V 供电及是否已切换到 I2C 模式 */
  } else {
      BringupDiag_SetJYRightResult(1, jy_right_ret);
  }
#endif
#if JY61P_LEFT_ENABLE
  jy_left_ret = JY61P_Init(JY61P_CH_LEFT);
  if (jy_left_ret != 0U) {
      BringupDiag_SetJYLeftResult(0, jy_left_ret);
      /* 左手 JY61P 未就绪：排查项同上 */
  } else {
      BringupDiag_SetJYLeftResult(1, jy_left_ret);
  }
#endif

  /* 阶段 4：初始化外设模块 */
#if DFPLAYER_ENABLE
  DFPlayer_Init();     /* DFPlayer 上电，默认音量 25 */
#endif
  BT_Init();           /* 蓝牙接收缓冲和协议解析器初始化 */
  Vibrator_Init();     /* TIM4 PWM 双通道启动，占空比先置 0 */
#if BUZZER_ENABLE
  /* 该模块为低电平触发：先预置高电平和 100% 高占空比，再使能 PWM。 */
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
  __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, BUZZER_PWM_SILENT_COMPARE);
  if (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_3) != HAL_OK) {
      Error_Handler();
  }
#endif
  Gesture_Init();      /* 手势状态机与 jerk 检测窗口清零 */
  SoftUART_Init();     /* PE0 软串口，预留给 ESP-01S MQTT */

  /* 阶段 5：初始化 MAX30102，失败时允许系统降级运行 */
  {
      uint8_t max30102_status = MAX30102_Init();
      if (max30102_status == 0U) {
          max30102_present = 1;
          BringupDiag_SetMAX30102Result(1, max30102_status);
      } else {
          max30102_present = 0;
          BringupDiag_SetMAX30102Result(0, max30102_status);
          /* MAX30102 不在线时，仅关闭心率/血氧功能，不阻塞其他模块 */
          /* 当前仍处于无实物阶段，这条降级路径也方便空板联调 */
      }
  }
  BringupDiag_RecomputeDegraded();

  /* 阶段 6：启动 UART3 单字节中断接收 */
  HAL_UART_Receive_IT(&huart3, (uint8_t *)&uart3_rx_byte, 1);
  BT_SendString("BOOT:C63AFB5\r\n");
  /* 诊断帧走同一条普通队列；主循环还会周期重发，不能依赖此一次。 */
  (void)BringupDiag_TrySend();

  /* 上电提示：当前关闭 DFPlayer，仅短振双手 */
#if DFPLAYER_ENABLE
  DFPlayer_Play(99);  /* 99.mp3 = 开机提示音 */
#endif
  Vibrator_Pulse(VIB_RIGHT, 50, 80);
  Vibrator_Pulse(VIB_LEFT,  50, 80);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = sys_tick_ms;

    /* 推进蓝牙发送队列；该调用不等待 UART。 */
    BT_TxService();

    if (alarm_buzzer_active != 0U &&
        (uint32_t)(now - alarm_buzzer_started_ms) >=
        ALARM_LOCAL_BUZZER_TIMEOUT_MS) {
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, BUZZER_PWM_SILENT_COMPARE);
        alarm_buzzer_active = 0U;
    }

    /*
     * Service the PPG FIFO before lower-priority polling and synchronous
     * UART telemetry. At 100 Hz, a 32-sample FIFO is full after only 320 ms.
     */
    if (max30102_present && (now - t_ppg_fifo >= 10U)) {
        t_ppg_fifo = now;
        MAX30102_ReadFIFO();
        now = sys_tick_ms;
    }

    /* 任务 1：10ms / 100Hz，读取已启用的 JY61P 通道。 */
#if JY61P_ENABLE
    if (now - t_jy_poll >= 10U) {
        t_jy_poll = now;
        float acc[3], gyro[3], angle[3];
        /*
         * Do not access an IMU which failed its power-on probe. An absent I2C
         * device can keep each transaction waiting for its timeout, starving
         * MAX30102 FIFO servicing. Reset after reconnecting an offline IMU so
         * that JY61P_Init() probes it again.
         */
#if JY61P_RIGHT_ENABLE
        if (JY61P_IsOnline(JY61P_CH_RIGHT)) {
            JY61P_Read_Data(JY61P_CH_RIGHT, acc, gyro, angle);
        } else if ((now - t_jy_right_recover) >= JY_RECOVERY_PERIOD_MS) {
            t_jy_right_recover = now;
            (void)JY61P_TryRecover(JY61P_CH_RIGHT);
        }
#endif
#if JY61P_LEFT_ENABLE
        if (JY61P_IsOnline(JY61P_CH_LEFT)) {
            JY61P_Read_Data(JY61P_CH_LEFT, acc, gyro, angle);
        }
#endif
    }
#endif

    /* I2C 读可能推进 TIM3 tick；所有下游任务使用读完成后的当前时间。 */
    now = sys_tick_ms;

    /* 任务 2：10ms / 100Hz，跌倒检测 + MAX30102 FIFO 读取 */
    if (now - t_10ms >= 10U) {
        t_10ms = now;

        /* 报警算法仅消费 JY61P 的新鲜 ACC + 姿态快照；不消费 CARE 演示值。 */
#if JY61P_ENABLE
        {
            Alarm_Sample_t alarm_sample;
            Alarm_Transition_t alarm_transition;
            uint32_t alarm_now = sys_tick_ms;

            alarm_sample.acc[0] = JY61P_Right.acc[0];
            alarm_sample.acc[1] = JY61P_Right.acc[1];
            alarm_sample.acc[2] = JY61P_Right.acc[2];
            alarm_sample.angle[0] = JY61P_Right.angle[0];
            alarm_sample.angle[1] = JY61P_Right.angle[1];
            alarm_sample.angle[2] = JY61P_Right.angle[2];
            alarm_sample.sample_tick_ms = JY61P_Right.acc_updated_ms;
            if ((JY61P_Right.angle_updated_ms -
                 alarm_sample.sample_tick_ms) < 0x80000000UL) {
                alarm_sample.sample_tick_ms = JY61P_Right.angle_updated_ms;
            }
            alarm_sample.valid =
                (JY61P_Right.acc_valid != 0U &&
                 JY61P_Right.angle_valid != 0U &&
                 Alarm_MainTimestampFresh(alarm_now,
                                          JY61P_Right.acc_updated_ms) != 0U &&
                 Alarm_MainTimestampFresh(alarm_now,
                                          JY61P_Right.angle_updated_ms) != 0U) ?
                1U : 0U;

            if (Alarm_Process(&alarm_state, &alarm_sample, alarm_now,
                              &alarm_transition) == ALARM_TRANSITION_RAISE) {
                Alarm_MainStartBuzzer(alarm_now);
                if (Alarm_MainSendEvent(&alarm_transition.event, 1U) != 0U) {
                    alarm_last_tx_ms = alarm_now;
                    alarm_tx_seen = 1U;
                } else {
                    /* 发送状态可知；活动事件仍锁存，100ms 任务会重试。 */
                    alarm_tx_seen = 0U;
                }
            }
        }
#endif

        /* MAX30102 FIFO 分时读取（100Hz 同步轮询） */
        if (max30102_present) {
            /* FIFO is serviced at the top of the loop. */
            /* 由驱动层负责 FIFO 溢出恢复、样本有效性与时间戳维护 */
        }
    }

    /* 任务 3：20ms / 50Hz，仅更新 Flex 数据；Flex 不参与蜂鸣器报警。 */
    if (now - t_20ms >= 20U) {
        t_20ms = now;
        /* v2.4 起改为 Ping-Pong 快照，Flex_Update 内部读取稳定半缓冲区 */
        Flex_Update();
    }

    /* 任务 4：50ms / 20Hz，手势识别 + 振动维护 */
    if (now - t_50ms >= 50U) {
        t_50ms = now;

        /* 根据当前双手状态机输出一个手势结果 */
        GestureResult_t gr = Gesture_Evaluate();
        if (gr.active) {
            if (gr.is_ctrl) {
                /* 控制模式：经软串口下发 MQTT 控制消息 */
                SoftUART_SendMQTT(ctrl_vocab[gr.ctrl_idx].topic,
                                  ctrl_vocab[gr.ctrl_idx].payload);
                Vibrator_Pulse(VIB_RIGHT, 50, 80);
            } else {
                /* 翻译/康复模式：当前仅振动反馈，DFPlayer 已关闭 */
#if DFPLAYER_ENABLE
                if (!DFPlayer_IsBusy()) {
                    DFPlayer_Play(gr.file_index);
                }
#endif
                Vibrator_Pulse(VIB_RIGHT, 50, 80);
                Vibrator_Pulse(VIB_LEFT,  50, 80);
            }
        }

        /* 维护振动脉冲自动关闭 */
        Vibrator_Tick();
    }

    /* 任务 5：100ms / 10Hz，模式切换 + 蓝牙指令 + PPG 批处理 */
    if (now - t_100ms >= 100U) {
        t_100ms = now;
        char bt_line[BT_RX_BUF_SIZE];

        if ((uint32_t)(now - t_bringup_diag) >= BRINGUP_DIAG_PERIOD_MS) {
            t_bringup_diag = now;
            (void)BringupDiag_TrySend();
        }

        /* 保持双手全弯 3 秒切换模式，并播放模式提示音 */
        {
            static uint8_t  mode_hold = 0;
            static uint32_t mode_start = 0;
            char cur[11];
            Gesture_GetCurrentCode(cur);
            cur[10] = '\0';
            if (strcmp(cur, "2222222222") == 0) {
                if (!mode_hold) {
                    mode_hold = 1;
                    mode_start = now;
                    /* Hold 期间冻结手势输出，避免误触发语音 */
                    Gesture_Freeze();
                } else if (now - mode_start > 3000U) {
                    /* 满足 3 秒后切换到下一模式 */
#if DFPLAYER_ENABLE
                    DFPlayer_Stop();
#endif
                    GestureMode_t m = Gesture_GetMode();
                    m = (GestureMode_t)(((uint8_t)m + 1U) % 3U);
                    Gesture_SetMode(m);
                    Gesture_Unfreeze();
#if DFPLAYER_ENABLE
                    if (m == MODE_TRANSLATE)      DFPlayer_Play(60);
                    else if (m == MODE_CONTROL)   DFPlayer_Play(61);
                    else                           DFPlayer_Play(62);
#endif
                    mode_hold = 0;
                }
            } else {
                /* 松手后解冻，恢复正常识别 */
                if (mode_hold) Gesture_Unfreeze();
                mode_hold = 0;
            }
        }

        /* 消费蓝牙指令 */
        uint8_t cmd = BT_GetCommand();
        switch (cmd) {
        case BT_CMD_CAL_MIN:
            Gesture_Calibrate(0, 0);  /* 右手 MIN 校准 */
            Gesture_Calibrate(1, 0);  /* 左手 MIN 校准 */
            break;
        case BT_CMD_CAL_MAX:
            Gesture_Calibrate(0, 1);  /* 右手 MAX 校准 */
            Gesture_Calibrate(1, 1);  /* 左手 MAX 校准 */
            break;
        case BT_CMD_SENS_1:
            Gesture_SetSensitivity(GESTURE_SENS_HIGH);
            break;
        case BT_CMD_SENS_2:
            Gesture_SetSensitivity(GESTURE_SENS_MED);
            break;
        case BT_CMD_SENS_3:
            Gesture_SetSensitivity(GESTURE_SENS_LOW);
            break;
        default:
            break;
        }

        if (BT_FetchLastString(bt_line, sizeof(bt_line))) {
#if DFPLAYER_ENABLE
            uint16_t file_num = 0U;
            unsigned volume = 0U;

            if (sscanf(bt_line, "<DF:PLAY=%hu>", &file_num) == 1) {
                DFPlayer_Play(file_num);
                BT_SendString("DF:PLAY OK\r\n");
            } else if (strstr(bt_line, "<DF:STOP>")) {
                DFPlayer_Stop();
                BT_SendString("DF:STOP OK\r\n");
            } else if (sscanf(bt_line, "<DF:VOL=%u>", &volume) == 1) {
                if (volume > 30U) {
                    volume = 30U;
                }
                DFPlayer_SetVolume((uint8_t)volume);
                BT_SendString("DF:VOL OK\r\n");
            }
#endif

            /* 仅匹配当前 BOOT 会话和当前活动 ID；过期 ACK 静默拒绝。 */
            {
                Alarm_Event_t cleared_event;
                if (Alarm_HandleAck(&alarm_state, bt_line,
                                    &cleared_event) != 0U) {
                    Alarm_MainStopBuzzer();
                    alarm_last_cleared_event = cleared_event;
                    alarm_last_cleared_valid = 1U;
                    alarm_clear_pending = 1U;
                    Alarm_MainRetryClear();
                    alarm_tx_seen = 0U;
                } else {
                    uint32_t ack_boot;
                    uint32_t ack_id;

                    /* 已解除事件只保留最近一条；完全匹配的重复 ACK
                       重新请求相同 ACTIVE=0，不触碰新的活动事件。 */
                    if (Alarm_ParseAck(bt_line, &ack_boot, &ack_id) != 0U &&
                        alarm_last_cleared_valid != 0U &&
                        ack_boot == alarm_last_cleared_event.boot_session &&
                        ack_id == alarm_last_cleared_event.id) {
                        alarm_clear_pending = 1U;
                        Alarm_MainRetryClear();
                    }
                }
            }
        }

        /* 队列满时消警回执不会丢失；每个 100ms 任务重试一次。 */
        Alarm_MainRetryClear();

        /* 活动报警周期重发，直到匹配 ACK 解除；发送本身由 BT 队列完成。 */
        {
            Alarm_Event_t active_event;
            if (Alarm_GetActive(&alarm_state, &active_event) != 0U) {
                if (alarm_tx_seen == 0U ||
                    (uint32_t)(now - alarm_last_tx_ms) >=
                    ALARM_REPEAT_PERIOD_MS) {
                    if (Alarm_MainSendEvent(&active_event, 1U) != 0U) {
                        alarm_last_tx_ms = now;
                        alarm_tx_seen = 1U;
                    } else {
                        /* 发送状态可知；下一次 100ms 任务继续尝试。 */
                        alarm_tx_seen = 0U;
                    }
                }
            } else {
                alarm_tx_seen = 0U;
            }
        }

        /* MAX30102 批处理仅更新展示数据，不得触发外部蜂鸣器或活动报警。 */
        if (max30102_present) {
            MAX30102_ProcessTick();

            /* 手指脱离提示：离线超过 2 秒时长振提醒重新佩戴 */
            {
                static uint8_t  finger_off_ticks = 0;
                static uint8_t  finger_off_alarmed = 0;
                if (!MAX30102_IsOnline()) {
                    finger_off_ticks++;
                    if (finger_off_ticks > 20 && !finger_off_alarmed) {
                        Vibrator_Pulse(VIB_RIGHT, 300, 50);
                        Vibrator_Pulse(VIB_LEFT,  300, 50);
                        finger_off_alarmed = 1;
                    }
                } else {
                    finger_off_ticks = 0;
                    finger_off_alarmed = 0;
                }
            }
        }

    }

    /* 任务 6：快速帧 4Hz；诊断帧 1Hz。为 9600bps 和报警队列留余量。 */
    now = sys_tick_ms;
    if (now - t_200ms >= BLUETOOTH_FAST_TELEMETRY_PERIOD_MS) {
#if MAX30102_DIAG_ONLY
    static uint32_t t_ppg_diag_tx = 0;
#endif
        t_200ms = now;
        uint8_t send_diag =
            ((uint32_t)(now - t_bt_diag) >= BLUETOOTH_DIAG_PERIOD_MS) ? 1U : 0U;
        if (send_diag != 0U) t_bt_diag = now;

        /* AA + 5 flex + 3 angles x 2 + HR/SpO2 + mode + 5 flex + XOR + BB. */
        uint8_t frame[22];
        uint8_t pos = 0;
        frame[pos++] = 0xAAU;  /* 帧头 */

        /* 右手 5 指弯曲百分比 */
        for (uint8_t f = 0; f < 5U; f++)
            frame[pos++] = Flex_GetPercent(0, f);

        /* Roll/Pitch/Yaw（乘 100，int16 小端） */
        int16_t roll  = (int16_t)(JY61P_Right.angle[0] * 100.0f);
        int16_t pitch = (int16_t)(JY61P_Right.angle[1] * 100.0f);
        int16_t yaw   = (int16_t)(JY61P_Right.angle[2] * 100.0f);
        frame[pos++] = (uint8_t)(roll & 0xFF);
        frame[pos++] = (uint8_t)((roll >> 8) & 0xFF);
        frame[pos++] = (uint8_t)(pitch & 0xFF);
        frame[pos++] = (uint8_t)((pitch >> 8) & 0xFF);
        frame[pos++] = (uint8_t)(yaw & 0xFF);
        frame[pos++] = (uint8_t)((yaw >> 8) & 0xFF);

        /* 心率和血氧 */
        frame[pos++] = max30102_present ? MAX30102_GetHR()   : 0U;
        frame[pos++] = max30102_present ? MAX30102_GetSpO2() : 0U;

        /* 当前模式 */
        frame[pos++] = (uint8_t)Gesture_GetMode();

        /* 左手 5 指弯曲百分比 */
        for (uint8_t f = 0; f < 5U; f++)
            frame[pos++] = Flex_GetPercent(1, f);

        /* XOR 校验（不含帧头和帧尾） */
        uint8_t csum = 0U;
        for (uint8_t i = 1U; i < pos; i++) csum ^= frame[i];
        frame[pos++] = csum;
        frame[pos++] = 0xBBU;

        {
#if !MAX30102_DIAG_ONLY
            char flex_line[128];
            char imu_line[64];
            char care_line[80];
#endif
            char jy_line[96];
            char acc_line[96];
            char ppg_line[96];
            char alarm_state_line[96];
#if MAX30102_DIAG_ONLY
            char ppgdbg_line[160];
            char ppgq_line[112];
#endif
            uint8_t hr_value = max30102_present ? MAX30102_GetHR() : 0U;
            uint8_t spo2_value = max30102_present ? MAX30102_GetSpO2() : 0U;
#if !MAX30102_DIAG_ONLY
            Alarm_Event_t current_alarm;
            uint8_t alarm_active_value =
                (Alarm_GetActive(&alarm_state, &current_alarm) != 0U) ? 1U : 0U;
            uint8_t fall_value =
                (alarm_active_value != 0U &&
                 current_alarm.type == ALARM_TYPE_FALL) ? 1U : 0U;
            uint8_t sos_value = alarm_active_value;
#endif
            uint8_t acc_valid_value = 0U;
            uint32_t ppg_ir_value = 0U;
            uint32_t ppg_red_value = 0U;
            uint8_t ppg_valid_value = 0U;
#if MAX30102_DIAG_ONLY
            MAX30102_DebugRegs_t ppg_dbg = {0};
            MAX30102_Quality_t ppg_quality = {0};
            uint8_t ppg_dbg_ret = 0U;
#endif

            if (max30102_buf_idx > 0U) {
                uint16_t last_idx = (uint16_t)(max30102_buf_idx - 1U);
                ppg_ir_value = max30102_buf[last_idx].ir;
                ppg_red_value = max30102_buf[last_idx].red;
                ppg_valid_value = max30102_buf[last_idx].valid;
            } else if (max30102_ready || max30102_buf[0].tick_ms > 0U) {
                ppg_ir_value = max30102_buf[MAX30102_BUF_LEN - 1U].ir;
                ppg_red_value = max30102_buf[MAX30102_BUF_LEN - 1U].red;
                ppg_valid_value = max30102_buf[MAX30102_BUF_LEN - 1U].valid;
            }

#if MAX30102_DIAG_ONLY
            if ((now - t_ppg_diag_tx) >= 1000U) {
                t_ppg_diag_tx = now;
                ppg_dbg_ret = max30102_present ? MAX30102_GetDebugRegs(&ppg_dbg) : 0U;
                (void)MAX30102_GetQuality(&ppg_quality);

                snprintf(ppg_line, sizeof(ppg_line),
                         "PPG|IR=%lu|RED=%lu|VALID=%u|HR=%u|SPO2=%u\r\n",
                         (unsigned long)ppg_ir_value,
                         (unsigned long)ppg_red_value,
                         ppg_valid_value,
                         hr_value,
                         spo2_value);
                BT_SendString(ppg_line);

                snprintf(ppgq_line, sizeof(ppgq_line),
                         "Q|N=%u|V=%u|ID=%lu|RD=%lu|IA=%lu|RA=%lu|HA=%lu|PK=%u|DT=%u|R=%u\r\n",
                         ppg_quality.sample_count,
                         ppg_quality.valid_samples,
                         (unsigned long)ppg_quality.ir_dc,
                         (unsigned long)ppg_quality.red_dc,
                         (unsigned long)ppg_quality.ir_pp,
                         (unsigned long)ppg_quality.red_pp,
                         (unsigned long)ppg_quality.hr_ac_pp,
                         ppg_quality.peak_intervals,
                         ppg_quality.median_interval_ms,
                         ppg_quality.ratio_x1000);
                BT_SendString(ppgq_line);

                snprintf(ppgdbg_line, sizeof(ppgdbg_line),
                         "DBG|W=%u|R=%u|O=%u|C=%u|X=%u|E=%u,%u,%u,%u|P=%u,%u,%u|U=%u|Z=%u\r\n",
                         ppg_dbg.wr_ptr,
                         ppg_dbg.rd_ptr,
                         ppg_dbg.ovf_cnt,
                         ppg_dbg.fifo_calls,
                         ppg_dbg.fifo_resets,
                         ppg_dbg.fifo_reg_errors,
                         ppg_dbg.fifo_data_errors,
                         ppg_dbg.fifo_reset_write_errors,
                         ppg_dbg.fifo_reset_verify_errors,
                         ppg_dbg.post_reset_wr_ptr,
                         ppg_dbg.post_reset_rd_ptr,
                         ppg_dbg.post_reset_ovf_cnt,
                         ppg_dbg.last_unread,
                         ppg_dbg_ret);
                BT_SendString(ppgdbg_line);
            }
#else
            snprintf(flex_line, sizeof(flex_line),
                     "FLEX|L1=%u|L2=%u|L3=%u|L4=%u|L5=%u|R1=%u|R2=%u|R3=%u|R4=%u|R5=%u\r\n",
                     Flex_GetPercent(1, 0), Flex_GetPercent(1, 1), Flex_GetPercent(1, 2),
                     Flex_GetPercent(1, 3), Flex_GetPercent(1, 4), Flex_GetPercent(0, 0),
                     Flex_GetPercent(0, 1), Flex_GetPercent(0, 2), Flex_GetPercent(0, 3),
                     Flex_GetPercent(0, 4));
            BT_SendString(flex_line);

            /*
             * angle_valid only describes the current JY61P transaction.  A
             * transient all-zero angle snapshot deliberately clears it, but
             * the driver keeps the last proven pose in angle[].  Keep that
             * pose visible to BLE after at least one good sample; the paired
             * JY|...|LAST=16 diagnostic tells the client that it is a
             * fallback display value and must not arm safety detection.
             */
            if (JY61P_Right.angle_sample_seen != 0U) {
                snprintf(imu_line, sizeof(imu_line),
                         "IMU|R=%.2f|P=%.2f|Y=%.2f\r\n",
                         (double)JY61P_Right.angle[0],
                         (double)JY61P_Right.angle[1],
                         (double)JY61P_Right.angle[2]);
                BT_SendString(imu_line);
            }

            if (send_diag != 0U) {
                snprintf(care_line, sizeof(care_line),
                         "CARE|HR=%u|SPO2=%u|FALL=%u|SOS=%u\r\n",
                         hr_value, spo2_value, fall_value, sos_value);
                BT_SendString(care_line);

                snprintf(ppg_line, sizeof(ppg_line),
                         "PPG|IR=%lu|RED=%lu|VALID=%u\r\n",
                         (unsigned long)ppg_ir_value,
                         (unsigned long)ppg_red_value,
                         ppg_valid_value);
                BT_SendString(ppg_line);
            }

#endif
            if (send_diag != 0U) {
                snprintf(jy_line, sizeof(jy_line),
                         "JY|ONLINE=%u|ERR=%u|LAST=%u|AGE=%lu\r\n",
                         JY61P_Right.online,
                         JY61P_Right.error_streak,
                         JY61P_Right.last_error,
                         (unsigned long)JY_MainAccAge(now, &JY61P_Right));
                BT_SendString(jy_line);

                /* VALID=0 仅用于诊断，不得进入报警判定。 */
                acc_valid_value =
                    (JY61P_Right.acc_valid != 0U &&
                     Alarm_MainTimestampFresh(now,
                                              JY61P_Right.acc_updated_ms) != 0U) ?
                    1U : 0U;
                snprintf(acc_line, sizeof(acc_line),
                         "ACC|X=%.3f|Y=%.3f|Z=%.3f|VALID=%u\r\n",
                         (double)JY61P_Right.acc[0],
                         (double)JY61P_Right.acc[1],
                         (double)JY61P_Right.acc[2],
                         acc_valid_value);
                BT_SendString(acc_line);

                if (Alarm_FormatState(alarm_state_line,
                                      sizeof(alarm_state_line),
                                      &alarm_state) > 0) {
                    BT_SendString(alarm_state_line);
                }
            }
        }

        BringupDiag_SetADCSeen(Flex_HasValidSnapshot(0), Flex_HasValidSnapshot(1));
    }

    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/*
 * TIM3 1ms 中断回调：维护全局系统 tick。
 * sys_tick_ms 是整套多速率调度器唯一的时间基准。
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        sys_tick_ms++;
    }
}

/*
 * USART3 接收完成回调：蓝牙单字节非阻塞接收。
 * 先重新挂接 HAL_UART_Receive_IT，再消费字节，避免回调期间丢下一个字节。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        uint8_t byte = uart3_rx_byte;
        HAL_UART_Receive_IT(&huart3, (uint8_t *)&uart3_rx_byte, 1);
        BT_RxCallback(byte);
    }
}

/*
 * USART1 DMA 发送完成回调：供 DFPlayer 串口 DMA 使用。
 * 完成后由 DFPlayer_DMA_TxCplt 释放忙标志并推进待发队列。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    BT_TxCpltCallback(huart);
#if DFPLAYER_ENABLE
    if (huart->Instance == USART1) {
        DFPlayer_DMA_TxCplt();
    }
#else
    (void)huart;
#endif
}

/* USER CODE END 4 */

/**
  * @brief System Clock Configuration
  * @retval None
  *
  * HSE 8MHz -> PLLM=8 / PLLN=336 / PLLP=2
  * SYSCLK = 8 / 8 * 336 / 2 = 168MHz
  * APB1 = 168 / 4 = 42MHz  （TIM3 时钟翻倍后为 84MHz）
  * APB2 = 168 / 2 = 84MHz  （TIM4 时钟为 84MHz）
  * ADC   = 84 / 4 = 21MHz  （低于 STM32F4 ADC 36MHz 上限）
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
   */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
