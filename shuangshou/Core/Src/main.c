/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body — 双手手语翻译手套 时序调度
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
#include "mpu6050.h"
#include "dfplayer.h"
#include "bluetooth.h"
#include "vibrator.h"
#include "gesture.h"
#include "stdio.h"
#include "string.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* ── ADC DMA 循环缓冲（每手 5 通道 × uint16_t） ── */
volatile uint16_t adc1_buf[5];  /* 右手: PA0~PA4 */
volatile uint16_t adc2_buf[5];  /* 左手: PA5~PA7, PB0~PB1 */

/* ── 时序调度时间戳 ── */
static uint32_t t_5ms   = 0;
static uint32_t t_10ms  = 0;
static uint32_t t_20ms  = 0;
static uint32_t t_50ms  = 0;
static uint32_t t_100ms = 0;
static uint32_t t_200ms = 0;

/* ── 全局 SOS 标志 ── */
static uint8_t sos_flag = 0;

/* ── 全局系统 tick ── */
volatile uint32_t sys_tick_ms = 0;

/* ── UART3 中断接收单字节缓冲 ── */
static volatile uint8_t uart3_rx_byte;
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
  /* USER CODE BEGIN 2 */

  /* ── 软件模块初始化 ── */
  SoftI2C_Init();
  Flex_Init();

  /* 启动 ADC DMA 循环扫描 */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc1_buf, 5);
  HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adc2_buf, 5);

  /* MPU6050 初始化（含陀螺仪零漂校准，每路约 200ms） */
  if (MPU6050_Init_Right() != 0) {
      /* 右手 MPU6050 初始化失败 → 蜂鸣报警 */
  }
  if (MPU6050_Init_Left() != 0) {
      /* 左手 MPU6050 初始化失败 → 蜂鸣报警 */
  }

  DFPlayer_Init();
  BT_Init();
  Vibrator_Init();
  Gesture_Init();

  /* 启动蓝牙中断接收 */
  HAL_UART_Receive_IT(&huart3, (uint8_t *)&uart3_rx_byte, 1);

  /* 开机提示音 */
  DFPlayer_Play(99);  /* 99.mp3 = 开机提示音 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = sys_tick_ms;

    /* ═══════════════════════════════════════════════
     * 任务 1: 5ms — MPU6050 原始数据读取 (200Hz)
     * ═══════════════════════════════════════════════ */
    if (now - t_5ms >= 5) {
        t_5ms = now;
        MPU6050_Read_All_Right();
        MPU6050_Read_All_Left();
    }

    /* ═══════════════════════════════════════════════
     * 任务 2: 10ms — 姿态解算 + 轨迹累积 + 跌倒检测 (100Hz)
     * ═══════════════════════════════════════════════ */
    if (now - t_10ms >= 10) {
        t_10ms = now;
        MPU6050_Compute_Attitude_Right();
        MPU6050_Compute_Attitude_Left();

        /* 跌倒检测 */
        if (MPU6050_FallDetect_Right() || MPU6050_FallDetect_Left()) {
            sos_flag = 1;
        }
    }

    /* ═══════════════════════════════════════════════
     * 任务 3: 20ms — 柔性传感器 (50Hz)
     * ═══════════════════════════════════════════════ */
    if (now - t_20ms >= 20) {
        t_20ms = now;
        Flex_Update();

        /* 手指痉挛检测 */
        for (uint8_t f = 0; f < 5; f++) {
            if (Flex_CheckSpasm(0, f) || Flex_CheckSpasm(1, f)) {
                sos_flag = 1;
            }
        }
    }

    /* ═══════════════════════════════════════════════
     * 任务 4: 50ms — 手势识别 + 振动状态维护 (20Hz)
     * ═══════════════════════════════════════════════ */
    if (now - t_50ms >= 50) {
        t_50ms = now;

        /* 手势判定 */
        GestureResult_t gr = Gesture_Evaluate();
        if (gr.active) {
            DFPlayer_Play(gr.file_index);
            Vibrator_Pulse(VIB_RIGHT, 50, 80);  /* 右手短振确认 */
            Vibrator_Pulse(VIB_LEFT,  50, 80);
        }

        /* 振动脉冲自动关断 */
        Vibrator_Tick();
    }

    /* ═══════════════════════════════════════════════
     * 任务 5: 100ms — 蓝牙指令处理 + SOS 处理 (10Hz)
     * ═══════════════════════════════════════════════ */
    if (now - t_100ms >= 100) {
        t_100ms = now;

        /* 蓝牙命令消费 */
        uint8_t cmd = BT_GetCommand();
        switch (cmd) {
        case BT_CMD_CAL_MIN:
            Gesture_Calibrate(0, 0);  /* 右手 min */
            Gesture_Calibrate(1, 0);  /* 左手 min */
            break;
        case BT_CMD_CAL_MAX:
            Gesture_Calibrate(0, 1);
            Gesture_Calibrate(1, 1);
            break;
        default:
            break;
        }

        /* SOS 触发 */
        if (sos_flag) {
            DFPlayer_Play(30);  /* 30.mp3 = SOS 语音 */
            sos_flag = 0;
        }
    }

    /* ═══════════════════════════════════════════════
     * 任务 6: 200ms — 蓝牙调试上报 (5Hz)
     * ═══════════════════════════════════════════════ */
    if (now - t_200ms >= 200) {
        t_200ms = now;

        /* 调试日志：输出右手五指百分比 */
        char dbg[64];
        snprintf(dbg, sizeof(dbg),
                 "<%.1f,%.1f,%d,%d,%d,%d,%d>\r\n",
                 MPU6050_Right.Pitch, MPU6050_Right.Roll,
                 Flex_GetPercent(0, 0), Flex_GetPercent(0, 1),
                 Flex_GetPercent(0, 2), Flex_GetPercent(0, 3),
                 Flex_GetPercent(0, 4));
        BT_SendString(dbg);
    }

    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/* ── TIM3 1ms 中断：维护全局系统 tick ── */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        sys_tick_ms++;
    }
}

/* ── USART 接收中断回调 ── */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        uint8_t byte = uart3_rx_byte;
        /* 先重新注册接收，再处理数据 */
        HAL_UART_Receive_IT(&huart3, (uint8_t *)&uart3_rx_byte, 1);
        BT_RxCallback(byte);
    }
}

/* ── USART1 DMA 发送完成回调 ── */
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
