#ifndef DATAPACK_RECEIVE_H
#define DATAPACK_RECEIVE_H

#include <Arduino.h>

// UART 引脚配置：按需求固定 RX=IO13，TX 默认使用 IO4。
#ifndef DATAPACK_UART_RX_PIN
#define DATAPACK_UART_RX_PIN 13
#endif

#ifndef DATAPACK_UART_TX_PIN
#define DATAPACK_UART_TX_PIN 4
#endif

#ifndef DATAPACK_UART_BAUD
#define DATAPACK_UART_BAUD 115200
#endif

extern uint8_t kNormalPower_t;

constexpr size_t DATAPACK_SIZE = 11;

void dataPackInitUart();
void dataPackPoll();

// 根据业务标志构建 11 字节数据包。
void dataPackBuild(uint8_t (&data_packet)[DATAPACK_SIZE], bool flag_hongwai_warning, bool flag_qingxie_warning);

// 通过 UART2 发送一帧数据包。
void dataPackSend(bool flag_hongwai_warning, bool flag_qingxie_warning);

// 主循环轮询标志：收到并成功解包一帧后置位。
bool dataPackTakeUpdatedFlag();

// 获取最近一次解包结果。
bool dataPackHasValidPacket();
uint8_t dataPackLatestWarningCode();
bool dataPackLatestIsHongwaiWarning();
bool dataPackLatestIsQingxieWarning();
bool dataPackCopyLatest(uint8_t (&out_packet)[DATAPACK_SIZE]);

#endif
