#ifndef EEPROM_H
#define EEPROM_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

/* AT24C02 存储布局（Tech_Design §阈值存储，8 字节 = 1 页，无需跨页）：
 *  0x00  2  魔数 0x47 0x48 ('G' 'H')
 *  0x02  2  th（大端）
 *  0x04  2  hys（大端）
 *  0x06  2  校验和 = 前 6 字节之和取低 16 位
 */
#define EEPROM_MAGIC0       0x47U
#define EEPROM_MAGIC1       0x48U

/* 魔数不符 / 校验失败 / 不变式不满足 → 回退默认值，不阻塞启动 */
bool EEPROM_Save(uint16_t th, uint16_t hys);
bool EEPROM_Load(uint16_t *th, uint16_t *hys);

#endif /* EEPROM_H */
