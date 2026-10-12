#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#include <stdint.h>

/* 调试串口（USART1 PA9/PA10 -> 板载 CH340C -> PC 的 COM 口）
 *
 * 用途：把 AT 交互与本地闭环状态打到串口，用于**上板诊断**。
 * 为什么必须有它：ESP8266 一旦连不上，现象只是"上位机收不到数据"，
 * 分不清是"模块没接/没供电"、"WiFi 连不上"、还是"TCP 连不上"。
 * 有串口就能直接看到模块到底回了什么。
 *
 * 由 main.h 的 DEBUG_UART 开关控制：
 *   0 = 编译为空实现（**发布固件默认**，不占串口、不影响时序）
 *   1 = 输出诊断信息（115200 8N1）
 * 生成工程时可用 `py tools/make_keil_project.py --debug` 自动打开。
 *
 * 前提：板子的 P3 跳线必须插上（PA9/PA10 才接到 CH340C）。
 */

void Dbg_Init(void);
void Dbg_Write(const char *s);
void Dbg_Printf(const char *fmt, ...);

#endif /* DEBUG_UART_H */
