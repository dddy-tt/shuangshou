/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 双手手语翻译手套 - 主调度框架（防御性重构 v3.1）
  * @author         : 项目维护重构版 v3.1
  * @date           : 2026-06-29
  * @note 多速率时序调度（基于 TIM3 1ms 系统 tick）
  *   5ms   / 200Hz : JY61P 双路 Burst Read（ACC + GYRO + ANGLE）
  *   10ms  / 100Hz : 跌倒检测 + MAX30102 FIFO 分时读取
  *   20ms  /  50Hz : ADC Ping-Pong 快照 + Flex_Update + 痉挛检测
  *   50ms  /  20Hz : 手势识别 + DFPlayer 触发 + 振动维护
  *   100ms /  10Hz : 蓝牙指令消费 + SOS 告警 + MAX30102 批处理
  *   200ms /   5Hz : 蓝牙遥测帧输出（手指/姿态/HR/SpO2/Mode）
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
#include "soft_i2c.h"
#include "flex_sensor.h"
#include "jy61p.h"
#include "dfplayer.h"
#include "bluetooth.h"
#include "vibrator.h"
#include "gesture.h"
#include "max30102.h"
#include "bringup_diag.h"
#include "soft_uart.h"
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

/* MAX30102 生命体征告警阈值 */
#define HR_ALARM_HIGH   120U   /* 心率 > 120 bpm -> 异常告警 */
#define HR_ALARM_LOW     50U   /* 心率 <  50 bpm -> 异常告警 */
#define SPO2_ALARM_LOW   90U   /* 血氧 <  90%   -> 缺氧告警 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* ADC DMA 循环缓冲（每只手 5 通道，Ping-Pong 双半区） */
volatile uint16_t adc1_buf[ADC_PINGPONG_SIZE];  /* 右手 5ch x 2 half-buffer */
volatile uint16_t adc2_buf[ADC_PINGPONG_SIZE];  /* 左手 5ch x 2 half-buffer */

/* 多速率调度时间戳 */
static uint32_t t_5ms   = 0;
static uint32_t t_10ms  = 0;
static uint32_t t_20ms  = 0;
static uint32_t t_50ms  = 0;
static uint32_t t_100ms = 0;
static uint32_t t_200ms = 0;

/* 全局 SOS 标志，可被多个任务置位 */
static uint8_t sos_flag = 0;

/* 全局系统 tick，由 TIM3 1ms 中断递增 */
volatile uint32_t sys_tick_ms = 0;

/* UART3 中断接收单字节缓冲 */
static volatile uint8_t uart3_rx_byte;

/* MAX30102 初始化状态 */
static uint8_t max30102_present = 0;  /* 1 = 设备在线且初始化成功 */

/* 三步跌倒检测状态机（基于 JY61P 加速度） */
static uint8_t  fall_state = 0;        /* 0=待机 1=失重 2=撞击后等待静止 */
static uint32_t fall_t_freefall = 0;
static uint32_t fall_t_impact = 0;

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
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  uint8_t jy_right_ret;
  uint8_t jy_left_ret;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
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
  SoftI2C_Init();      /* 三路软 I2C 总线复位并确认空闲 */
  Flex_Init();         /* 柔性传感器极值/方向初始化 */

  /* 阶段 2：启动 ADC DMA 循环扫描（双 ADC 独立 DMA） */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc1_buf, ADC_PINGPONG_SIZE);
  HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adc2_buf, ADC_PINGPONG_SIZE);

  /* 阶段 3：初始化双路 JY61P（已替代旧注释中的 MPU6050） */
  jy_right_ret = JY61P_Init(JY61P_CH_RIGHT);
  if (jy_right_ret != 0U) {
      BringupDiag_SetJYRightResult(0, jy_right_ret);
      /* 右手 JY61P 未就绪：检查 3.3V 供电及是否已切换到 I2C 模式 */
  } else {
      BringupDiag_SetJYRightResult(1, jy_right_ret);
  }
  jy_left_ret = JY61P_Init(JY61P_CH_LEFT);
  if (jy_left_ret != 0U) {
      BringupDiag_SetJYLeftResult(0, jy_left_ret);
      /* 左手 JY61P 未就绪：排查项同上 */
  } else {
      BringupDiag_SetJYLeftResult(1, jy_left_ret);
  }

  /* 阶段 4：初始化外设模块 */
  DFPlayer_Init();     /* DFPlayer 上电，默认音量 25 */
  BT_Init();           /* 蓝牙接收缓冲和协议解析器初始化 */
  Vibrator_Init();     /* TIM4 PWM 双通道启动，占空比先置 0 */
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

  /* 上电提示：播放启动音并短振双手 */
  DFPlayer_Play(99);  /* 99.mp3 = 开机提示音 */
  Vibrator_Pulse(VIB_RIGHT, 50, 80);
  Vibrator_Pulse(VIB_LEFT,  50, 80);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = sys_tick_ms;

    /* 任务 1：5ms / 200Hz，双路 JY61P Burst Read */
    if (now - t_5ms >= 5U) {
        t_5ms = now;
        float acc[3], gyro[3], angle[3];
        JY61P_Read_Data(JY61P_CH_RIGHT, acc, gyro, angle);
        JY61P_Read_Data(JY61P_CH_LEFT,  acc, gyro, angle);
    }

    /* 任务 2：10ms / 100Hz，跌倒检测 + MAX30102 FIFO 读取 */
    if (now - t_10ms >= 10U) {
        t_10ms = now;

        /* 三步跌倒检测（以右手加速度为主） */
        {
            float ax = JY61P_Right.acc[0];
            float ay = JY61P_Right.acc[1];
            float az = JY61P_Right.acc[2];
            float a_mag = sqrtf(ax*ax + ay*ay + az*az);

            switch (fall_state) {
            case 0: /* 待机 -> 监测失重 */
                if (a_mag < 3.92f) {  /* < 0.4g */
                    if (fall_t_freefall == 0) fall_t_freefall = now;
                    if (now - fall_t_freefall > 30) fall_state = 1;
                } else {
                    fall_t_freefall = 0;
                }
                break;
            case 1: /* 失重后等待撞击 */
                if (a_mag > 29.4f) {  /* > 3g */
                    fall_t_impact = now;
                    fall_state = 2;
                }
                if (now - fall_t_freefall > 2000) { fall_state = 0; fall_t_freefall = 0; }
                break;
            case 2: /* 撞击后等待静止确认 */
                if (now - fall_t_impact > 10000) {
                    if (a_mag < 11.76f && a_mag > 7.84f) {  /* 0.8g~1.2g = 静止躺地 */
                        sos_flag = 1;  /* 满足跌倒条件，触发 SOS */
                    }
                    fall_state = 0;
                }
                break;
            }
        }

        /* MAX30102 FIFO 分时读取（100Hz 同步轮询） */
        if (max30102_present) {
            MAX30102_ReadFIFO();
            /* 由驱动层负责 FIFO 溢出恢复、样本有效性与时间戳维护 */
        }
    }

    /* 任务 3：20ms / 50Hz，Flex 更新 + 手指痉挛检测 */
    if (now - t_20ms >= 20U) {
        t_20ms = now;
        /* v2.4 起改为 Ping-Pong 快照，Flex_Update 内部读取稳定半缓冲区 */
        Flex_Update();

        /* 基于更新后的历史缓冲进行痉挛检测 */
        for (uint8_t f = 0; f < 5; f++) {
            if (Flex_CheckSpasm(0, f) || Flex_CheckSpasm(1, f)) {
                sos_flag = 1;  /* 检出痉挛后统一走 SOS 提示链路 */
            }
        }
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
                /* 翻译/康复模式：播放对应语音 */
                if (!DFPlayer_IsBusy()) {
                    DFPlayer_Play(gr.file_index);
                }
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
                    /* 满足 3 秒后停播当前语音并切换到下一模式 */
                    DFPlayer_Stop();
                    GestureMode_t m = Gesture_GetMode();
                    m = (GestureMode_t)(((uint8_t)m + 1U) % 3U);
                    Gesture_SetMode(m);
                    Gesture_Unfreeze();
                    if (m == MODE_TRANSLATE)      DFPlayer_Play(60);
                    else if (m == MODE_CONTROL)   DFPlayer_Play(61);
                    else                           DFPlayer_Play(62);
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

        /* MAX30102 批处理与生命体征告警 */
        if (max30102_present) {
            MAX30102_ProcessTick();

            uint8_t hr   = MAX30102_GetHR();
            uint8_t spo2 = MAX30102_GetSpO2();

            if (hr > HR_ALARM_HIGH || (hr < HR_ALARM_LOW && hr > 0)) {
                sos_flag = 1;  /* 心率异常 -> SOS */
            }
            if (spo2 < SPO2_ALARM_LOW && spo2 > 0) {
                sos_flag = 1;  /* 血氧异常 -> SOS */
            }

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

        /* 统一 SOS 音频触发 */
        if (sos_flag) {
            DFPlayer_Play(30);  /* 30.mp3 = 紧急提示音 */
            sos_flag = 0;
        }
    }

    /* 任务 6：200ms / 5Hz，蓝牙遥测帧 */
    if (now - t_200ms >= 200U) {
        static uint32_t t_bringup_diag = 0;
        t_200ms = now;

        uint8_t frame[20];
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
            char flex_line[128];
            char imu_line[64];

            snprintf(flex_line, sizeof(flex_line),
                     "FLEX|L1=%u|L2=%u|L3=%u|L4=%u|L5=%u|R1=%u|R2=%u|R3=%u|R4=%u|R5=%u\r\n",
                     Flex_GetPercent(1, 0), Flex_GetPercent(1, 1), Flex_GetPercent(1, 2),
                     Flex_GetPercent(1, 3), Flex_GetPercent(1, 4), Flex_GetPercent(0, 0),
                     Flex_GetPercent(0, 1), Flex_GetPercent(0, 2), Flex_GetPercent(0, 3),
                     Flex_GetPercent(0, 4));
            BT_SendString(flex_line);

            snprintf(imu_line, sizeof(imu_line),
                     "IMU|R=%.2f|P=%.2f|Y=%.2f\r\n",
                     (double)JY61P_Right.angle[0],
                     (double)JY61P_Right.angle[1],
                     (double)JY61P_Right.angle[2]);
            BT_SendString(imu_line);
        }

        BringupDiag_SetADCSeen(Flex_HasValidSnapshot(0), Flex_HasValidSnapshot(1));
        if ((now - t_bringup_diag) >= 1000U) {
            t_bringup_diag = now;
            (void)BringupDiag_TrySend(&huart3);
        }
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
    if (huart->Instance == USART1) {
        DFPlayer_DMA_TxCplt();
    }
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
