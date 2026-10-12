#ifndef ESP8266_H
#define ESP8266_H

#include <stdint.h>
#include "main.h"

/* ===== 对外接口 ===== */
void Esp8266_Init(const char *ssid, const char *pwd, const char *ip, uint16_t port);
void Esp8266_Poll(void);                 /* 非阻塞：AT 状态机 + 退避重连，主循环每次调用 */
bool Esp8266_IsConnected(void);          /* 已进入透传且链路正常 */
void Esp8266_SendRaw(const char *data, uint16_t len);   /* 透传发送，未连接时丢弃 */

/* 串口接收入口：由 **USART3** 中断回调调用（经 ESP_IRQHandler 宏 →
 * HAL_UART_RxCpltCallback → 本函数；ISR 里只入缓冲，业务在 Poll 里做）。
 * 注：ESP8266 挂在 USART3（PB10/PB11），不是 USART2。 */
void Esp8266_UartRxByte(uint8_t b);

#endif /* ESP8266_H */
