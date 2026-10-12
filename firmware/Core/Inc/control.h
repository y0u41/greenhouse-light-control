#ifndef CONTROL_H
#define CONTROL_H

#include <stdint.h>
#include "main.h"

typedef enum { MODE_AUTO = 0, MODE_MANUAL = 1 } ctrl_mode_t;

/* ===== 执行器抽象（Tech_Design 附录 A.5）=====
 * 业务代码只认 Actuator_Init / Actuator_Set，不直接出现 GPIOE/GPIO_PIN_5/
 * GPIOD/GPIO_PIN_11。换硬件（LED1 等效负载 → 继电器 PD11）只改 control.c 里 4 行宏。 */
void Actuator_Init(void);
void Actuator_Set(uint8_t on);

/* 蜂鸣器：PB8，高电平响（官方 beep.c：BEEP(0)=关闭） */
void Buzzer_Init(void);
void Buzzer_Set(uint8_t on);

/* 控制核心 */
void Control_Init(void);        /* 执行器/蜂鸣器到安全态（不含参数装载） */
void Control_Apply(uint16_t th, uint16_t hys, uint8_t mode);   /* EEPROM 装载后调用 */
void Control_Loop(uint16_t light);                              /* 回差+手动优先，主循环每次调用 */
void Control_OnCommand(const char *action, int32_t value);      /* 下行指令入口 */

/* 状态读取（protocol 组包用） */
uint16_t Control_GetTh(void);
uint16_t Control_GetHys(void);
uint8_t  Control_GetMode(void);
uint8_t  Control_GetRelay(void);
uint8_t  Control_GetBuzz(void);

#endif /* CONTROL_H */
