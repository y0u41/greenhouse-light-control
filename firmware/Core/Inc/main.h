#ifndef __MAIN_H
#define __MAIN_H

#include "stm32f1xx_hal.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ===================== 系统配置 ===================== */

/* 节点 ID：本组单板用编译宏；多板后续改 Flash 配置项（Tech_Design §多点位） */
#ifndef NODE_ID
#define NODE_ID                 1U
#endif

/* 采集 / 上报周期：1 s，同时充当链路心跳（无需额外 ping/pong 帧） */
#define SYS_TICK_MS             1000U

/* LED0 运行心跳：每 5 s 翻转一次（5 s 亮 / 5 s 灭），程序活着的最直观证据 */
#define HEARTBEAT_TOGGLE_SEC    5U

/* 默认阈值 / 回差（light 单位；light = 4095 - adc，0 最暗 / 4095 最强） */
#define TH_DEFAULT              2000U
#define HYS_DEFAULT             150U

/* ★ 量程上限（12 位 ADC 满量程）：light 与 adc 的**唯一事实源**。
 * 历史上 control.c 与 eeprom.c 里共 4 处直接写 4095，而 adc_light.h 另有一份
 * LIGHT_ADC_MAX —— "单一事实源已经存在却被绕过"，改量程必然漏改。
 * 现在统一上移到本文件，阈值不变式 0 <= hys < th <= LIGHT_ADC_MAX 全部引用它。 */
#define LIGHT_ADC_MAX           4095U

/* 8 次均值滤波（自写循环，无 delay，≈0.17 ms） */
#ifndef ADC_LIGHT_AVG_TIMES
#define ADC_LIGHT_AVG_TIMES     8U
#endif

/* LCD 显示开关：1 = 使用官方 BSP lcd.h；0 = 编译桩（无 BSP 时本地闭环仍可运行） */
#ifndef LCD_SHOW_ENABLED
#define LCD_SHOW_ENABLED        1
#endif

/* 调试串口开关（USART1 PA9/PA10 -> CH340C，115200）：
 *   0 = 关（**发布固件默认**，空实现，不占串口）
 *   1 = 开，输出 AT 交互与本地闭环状态，用于**上板诊断**
 * 生成工程时用 `py tools/make_keil_project.py --debug` 可自动置 1。
 * 前提：P3 跳线插上，PA9/PA10 才接到 CH340C。 */
#ifndef DEBUG_UART
#define DEBUG_UART              0
#endif

/* 安全闪烁自检固件开关**已移除**（原 SAFE_BLINK_TEST）：
 *   曾在 main.c 里放一套绕过 HAL 的备用 main()（只开 GPIOB + 忙等翻转 PB5），
 *   用于把"MCU 有没有在跑我们的镜像"与"我们的初始化序列有问题"彻底分开。
 *   该分支从未被构建过（Keil 工程 Define 里始终没有它），属于**编译掉的死代码**，
 *   已整段删除。分诊思路见 docs/lessons.md#semihosting —— 现在 main() 开头的
 *   **裸寄存器三次闪灯**已能把"没进 main()"与"进 main 后在 HAL 里挂掉"分开，
 *   无需再维护第二套 main() 入口。 */

/* ===================== 全局状态 ===================== */
/* 唯一定义处：main.c。各业务模块通过 main.h 以 extern 访问。 */
extern uint16_t g_adc;     /* ADC 原始码值，越小越亮 */
extern uint16_t g_light;   /* 相对光强 = 4095 - adc */
extern uint16_t g_th;      /* 当前阈值（light 单位） */
extern uint16_t g_hys;     /* 当前回差（light 单位） */
extern uint8_t  g_relay;   /* 执行器状态 0/1 */
extern uint8_t  g_buzz;    /* 蜂鸣器状态 0/1 */
extern uint8_t  g_mode;    /* 控制模式 0=自动 1=手动 */

void Error_Handler(void);
void Iwdg_Feed(void);

#endif /* __MAIN_H */
