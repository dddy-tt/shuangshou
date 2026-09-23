#include "dfplayer.h"
#include "usart.h"
#include "string.h"

extern UART_HandleTypeDef huart1;

static uint8_t tx_frame[10];

static uint16_t calc_checksum(uint8_t cmd, uint8_t p1, uint8_t p2)
{
    return (uint16_t)(0xFFFFU + 1U - (0xFFU + 0x06U + cmd + p1 + p2));
}

static void fill_frame(uint8_t *frame, uint8_t cmd, uint8_t p1, uint8_t p2)
{
    uint16_t sum = calc_checksum(cmd, p1, p2);

    frame[0] = 0x7E;
    frame[1] = 0xFF;
    frame[2] = 0x06;
    frame[3] = cmd;
    frame[4] = 0x00;
    frame[5] = p1;
    frame[6] = p2;
    frame[7] = (uint8_t)(sum >> 8);
    frame[8] = (uint8_t)(sum & 0xFFU);
    frame[9] = 0xEF;
}

static void send_frame(uint8_t cmd, uint8_t p1, uint8_t p2)
{
    fill_frame(tx_frame, cmd, p1, p2);
    (void)HAL_UART_Transmit(&huart1, tx_frame, sizeof(tx_frame), 100U);
}

void DFPlayer_Init(void)
{
    DFPlayer_SetVolume(25U);
}

void DFPlayer_Play(uint16_t file_num)
{
    if (file_num == 0U || file_num > 9999U) {
        return;
    }
    send_frame(0x03U, (uint8_t)(file_num >> 8), (uint8_t)(file_num & 0xFFU));
}

void DFPlayer_Stop(void)
{
    send_frame(0x16U, 0x00U, 0x00U);
}

void DFPlayer_SetVolume(uint8_t vol)
{
    if (vol > 30U) {
        vol = 30U;
    }
    send_frame(0x06U, 0x00U, vol);
}

uint8_t DFPlayer_IsBusy(void)
{
    return 0U;
}

void DFPlayer_DMA_TxCplt(void)
{
    /* 阻塞发送版本保留空实现，避免影响现有回调链路 */
}
