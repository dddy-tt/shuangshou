#ifndef __DFPLAYER_H
#define __DFPLAYER_H

#include "stdint.h"

/*
 * 初始化：无额外硬件操作，仅记录 UART 句柄
 */
void DFPlayer_Init(void);

/*
 * ⚠️ 非阻塞播放指定文件（DMA 发送）
 * 调用后立即返回，不等待发送完毕
 *
 * file_num: SD 卡根目录文件编号 (1~9999)
 *           0001.mp3 → file_num=1
 *
 * ⚠️ 严禁在 ISR 上下文中调用
 */
void DFPlayer_Play(uint16_t file_num);

/*
 * 停止播放
 */
void DFPlayer_Stop(void);

/*
 * 设置音量
 * vol: 0~30，默认 25
 */
void DFPlayer_SetVolume(uint8_t vol);

/*
 * 查询当前是否正在 DMA 发送指令帧
 * 返回 0=空闲, 1=发送中
 */
uint8_t DFPlayer_IsBusy(void);

/* DMA 发送完成回调（由 HAL_UART_TxCpltCallback 调用，勿手动调用） */
void DFPlayer_DMA_TxCplt(void);

#endif /* __DFPLAYER_H */
