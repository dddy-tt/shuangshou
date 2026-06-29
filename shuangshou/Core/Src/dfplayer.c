#include "dfplayer.h"
#include "usart.h"
#include "string.h"

/* ── DFPlayer 指令帧格式 ── */
/* 帧: [7E][FF][06][CMD][ACK][P1][P2][CHK_H][CHK_L][EF] */
/* 固定 10 字节，CHK = 0 - (VERSION+LEN+CMD+ACK+P1+P2) */

extern UART_HandleTypeDef huart1;

static volatile uint8_t dma_busy = 0;

/* ── 发送帧（DMA 非阻塞） ── */
static void send_frame(uint8_t cmd, uint8_t p1, uint8_t p2)
{
    uint8_t frame[10];
    frame[0] = 0x7E;
    frame[1] = 0xFF;
    frame[2] = 0x06;
    frame[3] = cmd;
    frame[4] = 0x00;  /* ACK: 0=不需要应答 */
    frame[5] = p1;
    frame[6] = p2;
    uint16_t sum = 0xFFFF + 1 - (0xFF + 0x06 + cmd + 0x00 + p1 + p2);
    frame[7] = (uint8_t)(sum >> 8);
    frame[8] = (uint8_t)(sum & 0xFF);
    frame[9] = 0xEF;

    dma_busy = 1;
    HAL_UART_Transmit_DMA(&huart1, frame, 10);
}

/* ── 公共 API ── */

void DFPlayer_Init(void)
{
    dma_busy = 0;
    DFPlayer_SetVolume(25);
}

void DFPlayer_Play(uint16_t file_num)
{
    if (file_num == 0 || file_num > 9999) return;
    send_frame(0x03, (uint8_t)(file_num >> 8), (uint8_t)(file_num & 0xFF));
}

void DFPlayer_Stop(void)
{
    send_frame(0x16, 0x00, 0x00);
}

void DFPlayer_SetVolume(uint8_t vol)
{
    if (vol > 30) vol = 30;
    send_frame(0x06, 0x00, vol);
}

uint8_t DFPlayer_IsBusy(void)
{
    return dma_busy;
}

/* ── DMA 传输完成回调（在 stm32f4xx_it.c 中调用） ── */
void DFPlayer_DMA_TxCplt(void)
{
    dma_busy = 0;
}
