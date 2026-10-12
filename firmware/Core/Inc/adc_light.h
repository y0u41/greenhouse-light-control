#ifndef ADC_LIGHT_H
#define ADC_LIGHT_H

#include <stdint.h>
#include "main.h"

/* 光照方向约定（Tech_Design 附录 A.2）：
 *   光照增强 → 光敏阻值下降 → PF8 电压下降 → ADC 码值减小
 * 因此用相对光强指标统一语义，阈值判断只建立在 light 上。 */
/* LIGHT_ADC_MAX 已上移到 main.h，作为全工程唯一事实源（control.c / eeprom.c 同样引用） */
#define LIGHT_FROM_ADC(a)    ((uint16_t)(LIGHT_ADC_MAX - (a)))   /* 0=全暗, 4095=最强 */

/* 硬件事实：板载光敏 LS1 → PF8 = ADC3_IN6（官方 lsens.h）。
 * 注意官方"实验20 readme.txt"写成 通道6(PA1) 是笔误，PA1 是 TPAD/电位器，与光照无关。 */

void     LightSensor_Init(void);          /* GPIOF 时钟 + PF8 模拟输入 + ADC3 初始化 + 校准 */
uint16_t Light_ReadRaw(uint8_t times);    /* times 次采样取均值（每次 ≈21 µs） */
uint8_t  Light_IsValid(void);             /* 1 = 上次读数可信；0 = 采样失败（勿据此决策） */

#endif /* ADC_LIGHT_H */
