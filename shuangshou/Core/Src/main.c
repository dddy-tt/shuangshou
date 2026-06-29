/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 双手手语翻译手套 — 主调度框架 (防御性重构 v2.3)
  * @author         : 专家评审重构版 v2.3
  * @date           : 2026-06-29
  *
  * @多速率时序调度 (6 级, 基于 TIM3 1ms 系统 tick):
  *   ┌──────────┬───────────┬──────────────────────────────────────────┐
  *   │ 周期     │ 频率      │ 任务内容                                  │
  *   ├──────────┼───────────┼──────────────────────────────────────────┤
  *   │  5ms     │ 200Hz     │ MPU6050 双路 14 字节读取 (软件 I2C)       │
  *   │ 10ms     │ 100Hz     │ 互补滤波姿态解算 + 跌倒检测                │
  *   │          │           │ MAX30102 FIFO 分时读取 (每 10ms 1 样本)    │
  *   │ 20ms     │ 50Hz      │ ★ ADC DMA 暂停快照 + 柔性传感器更新        │
  *   │          │           │ + 手指痉挛检测                             │
  *   │ 50ms     │ 20Hz      │ 手势识别 (手形编码 + 门限微分法方向)       │
  *   │          │           │ + DFPlayer 播放触发 + 振动维护              │
  *   │100ms     │ 10Hz      │ 蓝牙指令消费 + SOS 报警 + MAX30102 PPG     │
  *   │          │           │ 批量处理 + 心率/血氧超阈值检测              │
  *   │200ms     │  5Hz      │ 蓝牙调试遥测 (Pitch/Roll/五指百分比)       │
  *   └──────────┴───────────┴──────────────────────────────────────────┘
  *
  * @防御性设计 (v2.3 新增):
  *   ② ADC DMA 原子快照: 20ms 任务中, 先把 ADC_DMA 两个流 EN 位清 0,
  *      冻结缓冲数组后再调用 Flex_Update, 杜绝高动态下的数据撕裂
  *   ③ MAX30102 100Hz 对齐: 10ms 任务中调用 MAX30102_ReadFIFO,
  *      读取前检查 FIFO_NUM_SAMPLES, 自动处理溢出复位
  *   ⑦ PPG 边界卡关: 100ms 任务中若 IR < 5000 或 > 200000,
  *      MAX30102 内部自动阻断算法, main 中仅读取最终 HR/SpO2
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
#include "math.h"
#include "stdio.h"
#include "string.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/**
 * ★ v2.4 升级: 删除 ADC_DMA_PAUSE/RESUME 宏。
 *   改用 HAL_ADC_ConvHalfCpltCallback (前半组) 和 ConvCpltCallback (后半组)
 *   的 Ping-Pong 无锁双缓冲快照, 参见 flex_sensor.c 和 adc.c 的 USER CODE。 */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ── MAX30102 心率/血氧报警阈值 ── */
#define HR_ALARM_HIGH   120U   /* 心率 > 120 bpm → 异常报警 */
#define HR_ALARM_LOW     50U   /* 心率 <  50 bpm → 异常报警 */
#define SPO2_ALARM_LOW   90U   /* 血氧 <  90%   → 缺氧报警 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* ── ADC DMA 循环缓冲 (每手 5 通道 × uint16_t) ── */
volatile uint16_t adc1_buf[ADC_PINGPONG_SIZE];  /* 右手 5ch × 2 组 Ping-Pong */
volatile uint16_t adc2_buf[ADC_PINGPONG_SIZE];  /* 左手 5ch × 2 组 */

/* ── 时序调度时间戳 ── */
static uint32_t t_5ms   = 0;
static uint32_t t_10ms  = 0;
static uint32_t t_20ms  = 0;
static uint32_t t_50ms  = 0;
static uint32_t t_100ms = 0;
static uint32_t t_200ms = 0;

/* ── 全局 SOS 标志 (可被多个任务置位) ── */
static uint8_t sos_flag = 0;

/* ── 全局系统 tick (TIM3 1ms 中断递增) ── */
volatile uint32_t sys_tick_ms = 0;

/* ── UART3 中断接收单字节缓冲 ── */
static volatile uint8_t uart3_rx_byte;

/* ── MAX30102 初始化状态 ── */
static uint8_t max30102_present = 0;  /* 1=传感器初始化成功 */

/* ── 三步跌倒检测状态 (基于 JY61P 加速度) ── */
static uint8_t  fall_state = 0;        /* 0=正常, 1=失重, 2=撞击 */
static uint32_t fall_t_freefall = 0;
static uint32_t fall_t_impact = 0;

/* ── JY61P 全局数据引用 (外部声明) ── */
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

  /* ═══════════════════════════════════════════════════════════════
   * 阶段 1: 通信层初始化
   * ═══════════════════════════════════════════════════════════════ */
  SoftI2C_Init();     /* 三路软 I2C 总线复位 + 空闲确认 */
  Flex_Init();         /* 柔性传感器极值反向初始化 */

  /* ═══════════════════════════════════════════════════════════════
   * 阶段 2: 启动 ADC DMA 循环扫描 (双 ADC 独立 DMA 流)
   * ═══════════════════════════════════════════════════════════════ */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc1_buf, ADC_PINGPONG_SIZE);
  HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adc2_buf, ADC_PINGPONG_SIZE);

  /* ═══════════════════════════════════════════════════════════════
   * 阶段 3: JY61P 惯性传感器初始化 (零硬件改线, 直替原 MPU6050 位置)
   * ═══════════════════════════════════════════════════════════════ */
  if (JY61P_Init(JY61P_CH_RIGHT) != 0) {
      /* 右手 JY61P 不在线 — 检查: ①供电 3.3V? ②PC上位机已切I2C模式? */
  }
  if (JY61P_Init(JY61P_CH_LEFT) != 0) {
      /* 左手 JY61P 不在线 — 同上 */
  }

  /* ═══════════════════════════════════════════════════════════════
   * 阶段 4: 外设模块初始化
   * ═══════════════════════════════════════════════════════════════ */
  DFPlayer_Init();     /* DFPlayer 开机 + 默认音量 25 */
  BT_Init();           /* 蓝牙环形缓冲 + 协议解析器初始化 */
  Vibrator_Init();     /* TIM4 PWM 双通道启动 (占空比 = 0) */
  Gesture_Init();      /* 手势状态机 + Jerk 滑动窗口清零 */

  /* ═══════════════════════════════════════════════════════════════
   * 阶段 5: MAX30102 心率血氧传感器初始化
   * ═══════════════════════════════════════════════════════════════ */
  {
      uint8_t max30102_status = MAX30102_Init();
      if (max30102_status == 0) {
          max30102_present = 1;
      } else {
          max30102_present = 0;
          /* MAX30102 不在线 — 系统降级运行, 无心率血氧功能 */
          /* 不阻塞主程序, 其他所有功能正常运作 */
      }
  }

  /* ═══════════════════════════════════════════════════════════════
   * 阶段 6: 启动 UART 中断接收
   * ═══════════════════════════════════════════════════════════════ */
  HAL_UART_Receive_IT(&huart3, (uint8_t *)&uart3_rx_byte, 1);

  /* ── 开机确认: 播放提示音 + 短振 ── */
  DFPlayer_Play(99);  /* 99.mp3 = "系统已启动" */
  Vibrator_Pulse(VIB_RIGHT, 50, 80);
  Vibrator_Pulse(VIB_LEFT,  50, 80);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = sys_tick_ms;

    /* ═══════════════════════════════════════════════════════════
     * 任务 1:  5ms — JY61P Burst Read ACC+GYRO+ANGLE (200Hz)
     *
     * 每次耗时: 右 ~560μs + 左 ~560μs ≈ 1120μs @ 400kHz
     * 占时隙比: 1120μs / 5000μs = 22.4%
     *
     * ★ 一次 Burst Read 即获得: 加速度(m/s²) + 角速度(°/s) + 欧拉角(°)
     * ★ 不需互补滤波、不需零漂校准 — JY61P 内部卡尔曼已完成
     * ═══════════════════════════════════════════════════════════ */
    if (now - t_5ms >= 5U) {
        t_5ms = now;
        float acc[3], gyro[3], angle[3];
        JY61P_Read_Data(JY61P_CH_RIGHT, acc, gyro, angle);
        JY61P_Read_Data(JY61P_CH_LEFT,  acc, gyro, angle);
    }

    /* ═══════════════════════════════════════════════════════════
     * 任务 2: 10ms — 跌倒检测 + MAX30102 FIFO 分时读取 (100Hz)
     *
     * 跌倒: 三步判定法 (失重→撞击→静止), 基于 JY61P 加速度
     *       数据源: JY61P_Right.acc[] / JY61P_Left.acc[] (m/s²)
     * MAX30102: 读取 1 个 FIFO 样本 (6 字节, ~195μs @ 400kHz)
     *           ★ 采样率 100Hz 严格对齐 10ms 轮询 — 无 FIFO 失步溢出
     *           ★ 内部检查 FIFO_NUM_SAMPLES + 溢出标志
     * ═══════════════════════════════════════════════════════════ */
    if (now - t_10ms >= 10U) {
        t_10ms = now;

        /* ── 三步跌倒检测 (右手加速度为主) ── */
        {
            float ax = JY61P_Right.acc[0];
            float ay = JY61P_Right.acc[1];
            float az = JY61P_Right.acc[2];
            float a_mag = sqrtf(ax*ax + ay*ay + az*az);

            switch (fall_state) {
            case 0: /* 正常 → 检测失重 */
                if (a_mag < 3.92f) {  /* < 0.4g */
                    if (fall_t_freefall == 0) fall_t_freefall = now;
                    if (now - fall_t_freefall > 30) fall_state = 1;
                } else {
                    fall_t_freefall = 0;
                }
                break;
            case 1: /* 失重中 → 检测撞击 */
                if (a_mag > 29.4f) {  /* > 3g */
                    fall_t_impact = now;
                    fall_state = 2;
                }
                if (now - fall_t_freefall > 2000) { fall_state = 0; fall_t_freefall = 0; }
                break;
            case 2: /* 撞击后 → 静止确认 */
                if (now - fall_t_impact > 10000) {
                    if (a_mag < 11.76f && a_mag > 7.84f) {  /* 0.8g~1.2g = 静止躺地 */
                        sos_flag = 1;  /* 跌倒触发 SOS */
                    }
                    fall_state = 0;
                }
                break;
            }
        }

        /* ── MAX30102 FIFO 分时读取 (100Hz 同频轮询) ── */
        if (max30102_present) {
            MAX30102_ReadFIFO();
            /* ReadFIFO 内部:
             *   ① 读 FIFO_RD_PTR / FIFO_WR_PTR → 计算可用样本数
             *   ② 读 OVF_COUNTER → 若溢出 > 0 则 FIFO 复位
             *   ③ 读 6 字节 (RED+IR)
             *   ④ 边界卡关: IR ∈ [5000, 200000] → valid=1, 否则阻断
             *   ⑤ 有效样本入环形缓冲 → 累积 DC 均值
             * 耗时约 195μs @ 400kHz, 占 10ms 时隙的 1.95% */
        }
    }

    /* ═══════════════════════════════════════════════════════════
     * 任务 3: 20ms — ★ ADC DMA 原子快照 + 柔性传感器更新 (50Hz)
     *
     * ★★★ 数据撕裂防御 (v2.3 新增) ★★★
     *   问题: ADC DMA 循环模式在后台持续覆盖 adc_buf[0..4],
     *         Flex_Update 非原子性地顺次读取 10 个通道,
     *         可能在一次调用中读到半新半旧的数据。
     *   解决: 在调用 Flex_Update 前, 同时停掉两个 ADC 的 DMA 流,
     *         确保 adc_buf 数组在读取期间冻结不变。
     *   代价: 丢失约 2~3 个 ADC 采样周期 (~45μs),
     *         对 50Hz 更新率无实质影响。
     * ═══════════════════════════════════════════════════════════ */
    if (now - t_20ms >= 20U) {
        t_20ms = now;

        /* ── ★ v2.4 Ping-Pong 快照: Flex_Update 内部读取 HalfCplt/Cplt
         *    锁定的稳定半组, 无需暂停 DMA, 零撕裂保证 ★ ── */
        Flex_Update();

        /* ── 手指痉挛检测 (基于 Flex_Update 更新后的历史缓冲) ── */
        for (uint8_t f = 0; f < 5; f++) {
            if (Flex_CheckSpasm(0, f) || Flex_CheckSpasm(1, f)) {
                sos_flag = 1;  /* 手指痉挛 → 触发 SOS */
            }
        }
    }

    /* ═══════════════════════════════════════════════════════════
     * 任务 4: 50ms — 手势识别 + 振动维护 (20Hz)
     *
     * Gesture_Evaluate 内部:
     *   ① encode_fingers() — 10 指百分比 → 三态编码 (0/1/2)
     *      ★ 含百分比 [0,100] 双限硬钳位
     *   ② jerk_detect() — 门限微分法方向检测
     *      ★ 含冷启动计数保护 (前 20 样本静默)
     *      ★ 含旋转熔断 (角速度 >60°/s 时冻结)
     *   ③ 手势状态机: WAIT_STABLE → WAIT_TRAJ → ARMED → COOLDOWN
     *   ④ 查表匹配 → 返回 GestureResult_t
     * ═══════════════════════════════════════════════════════════ */
    if (now - t_50ms >= 50U) {
        t_50ms = now;

        /* ── 手势判定 ── */
        GestureResult_t gr = Gesture_Evaluate();
        if (gr.active) {
            /* 若 DFPlayer 正在播放, DMA 忙则跳过 (下一周期重试) */
            if (!DFPlayer_IsBusy()) {
                DFPlayer_Play(gr.file_index);
            }
            /* 振动反馈: 双手同时短振 50ms 确认识别 */
            Vibrator_Pulse(VIB_RIGHT, 50, 80);
            Vibrator_Pulse(VIB_LEFT,  50, 80);
        }

        /* ── 振动脉冲自动关断检查 ── */
        Vibrator_Tick();
    }

    /* ═══════════════════════════════════════════════════════════
     * 任务 5: 100ms — 蓝牙指令 + SOS 处理 + MAX30102 PPG 处理 (10Hz)
     *
     * MAX30102 PPG 批量处理: 此时环形缓冲已累积 ~10 个 100Hz 样本,
     *   ppg_process() 内部:
     *     ① DC 均值估算
     *     ② 自适应峰值间期检测 → HR
     *     ③ R_ratio 估算 → SpO2
     *   ★ 若手指脱离/饱和, 内部历史已清零, HR/SpO2 归 0
     * ═══════════════════════════════════════════════════════════ */
    if (now - t_100ms >= 100U) {
        t_100ms = now;

        /* ── 蓝牙命令消费 ── */
        uint8_t cmd = BT_GetCommand();
        switch (cmd) {
        case BT_CMD_CAL_MIN:
            Gesture_Calibrate(0, 0);  /* 右手 MIN 标定 */
            Gesture_Calibrate(1, 0);  /* 左手 MIN 标定 */
            break;
        case BT_CMD_CAL_MAX:
            Gesture_Calibrate(0, 1);  /* 右手 MAX 标定 */
            Gesture_Calibrate(1, 1);  /* 左手 MAX 标定 */
            break;
        default:
            break;
        }

        /* ── MAX30102 PPG 批量处理 + 生命体征报警 ── */
        if (max30102_present) {
            MAX30102_ProcessTick();    /* 内部调 ppg_process() */

            /* 读取处理后的心率血氧结果 */
            uint8_t hr   = MAX30102_GetHR();
            uint8_t spo2 = MAX30102_GetSpO2();

            /* ★ 生命体征超阈值检测 ★ */
            if (hr > HR_ALARM_HIGH || (hr < HR_ALARM_LOW && hr > 0)) {
                sos_flag = 1;  /* 心率异常 → SOS */
            }
            if (spo2 < SPO2_ALARM_LOW && spo2 > 0) {
                sos_flag = 1;  /* 血氧过低 → SOS */
            }

            /* 手指脱离提示: 传感器离线 > 2 秒 → 振动提醒 */
            {
                static uint8_t  finger_off_ticks = 0;
                static uint8_t  finger_off_alarmed = 0;
                if (!MAX30102_IsOnline()) {
                    finger_off_ticks++;
                    if (finger_off_ticks > 20 && !finger_off_alarmed) {
                        /* 连续 2 秒离线 → 长振提醒配戴 */
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

        /* ── SOS 全局触发汇总 ── */
        if (sos_flag) {
            DFPlayer_Play(30);  /* 30.mp3 = "需要帮助 / 检测到异常" */
            sos_flag = 0;
        }
    }

    /* ═══════════════════════════════════════════════════════════
     * 任务 6: 200ms — 蓝牙调试遥测 (5Hz)
     *
     * 上报格式: <Pitch,Roll,R0,R1,R2,R3,R4>\r\n
     * 示例:     <-12.3,5.8,45,12,78,3,91>
     * 数据可用于: 上位机波形显示 / 标定辅助 / 算法参数调优
     * ═══════════════════════════════════════════════════════════ */
    if (now - t_200ms >= 200U) {
        t_200ms = now;

        char dbg[96];
        int len = snprintf(dbg, sizeof(dbg),
                 "<R:%.1f,P:%.1f,Y:%.1f | %d,%d,%d,%d,%d>\r\n",
                 JY61P_Right.angle[0], JY61P_Right.angle[1], JY61P_Right.angle[2],
                 Flex_GetPercent(0, 0), Flex_GetPercent(0, 1),
                 Flex_GetPercent(0, 2), Flex_GetPercent(0, 3),
                 Flex_GetPercent(0, 4));
        if (len > 0 && len < (int)sizeof(dbg)) {
            BT_SendString(dbg);
        }

        /* ── 附加 MAX30102 生命体征遥测 (若传感器在线) ── */
        if (max30102_present) {
            uint8_t hr   = MAX30102_GetHR();
            uint8_t spo2 = MAX30102_GetSpO2();
            uint8_t online = MAX30102_IsOnline();
            char vitals[32];
            int vlen = snprintf(vitals, sizeof(vitals),
                     "<VITALS:%d,%d,%d>\r\n", hr, spo2, online);
            if (vlen > 0 && vlen < (int)sizeof(vitals)) {
                BT_SendString(vitals);
            }
        }
    }

    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/* ═══════════════════════════════════════════════════════════════
 * TIM3 1ms 中断回调 — 维护全局系统 tick
 *
 * sys_tick_ms 是整个多速率调度器的唯一时钟源。
 * TIM3 配置: PSC=83, ARR=999 → 84MHz / 84 / 1000 = 1kHz 溢出率
 * ═══════════════════════════════════════════════════════════════ */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        sys_tick_ms++;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * USART 接收中断回调 — 蓝牙单字节非阻塞接收
 *
 * 操作顺序至关重要: 必须先 HAL_UART_Receive_IT 重新注册,
 * 再调用 BT_RxCallback 消费数据。因为 BT_RxCallback 可能
 * 调 BT_SendString 使用 USART3 发送, 若先消费后注册,
 * 发送完毕的 TXE 中断可能误触发 RX 路径。
 * ═══════════════════════════════════════════════════════════════ */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        uint8_t byte = uart3_rx_byte;
        /* 先重新注册, 防止在 BT_RxCallback 中丢失下一个字节 */
        HAL_UART_Receive_IT(&huart3, (uint8_t *)&uart3_rx_byte, 1);
        BT_RxCallback(byte);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * USART1 DMA 发送完成回调 — DFPlayer 非阻塞发送完成通知
 *
 * DFPlayer Mini 指令帧为 10 字节, DMA 传输完毕后硬件触发此回调。
 * DFPlayer_DMA_TxCplt 清除 dma_busy 标志, 允许下一帧发送。
 * ═══════════════════════════════════════════════════════════════ */
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
  * HSE 8MHz → PLLM=8 / PLLN=336 / PLLP=2
  * SYSCLK = 8 / 8 × 336 / 2 = 168MHz
  * APB1 = 168 / 4 = 42MHz  (TIM3 时钟 = 84MHz 因为 APB1 prescaler > 1)
  * APB2 = 168 / 2 = 84MHz  (TIM4 时钟 = 84MHz)
  * ADC   = 84 / 4 = 21MHz  (不可超过 36MHz, 符合手册限制)
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
