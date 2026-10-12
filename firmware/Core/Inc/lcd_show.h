#ifndef LCD_SHOW_H
#define LCD_SHOW_H

#include <stdint.h>
#include "main.h"

void LcdShow_Init(void);
/* 本地液晶：光强值、阈值、执行器状态、控制模式 */
void LcdShow_Update(uint16_t light, uint16_t th, uint8_t relay, uint8_t mode);

#endif /* LCD_SHOW_H */
