/*
 * EEPROM 套件专用桩：伪 AT24C02（256 B）+ 可注入的故障模型。
 *
 * 为什么需要它（N-1 的软件侧复现）：
 *   真实 AT24C02 页写入后有 ≤5 ms 内部写周期，期间对**地址探测** NACK；
 *   HAL_I2C_IsDeviceReady 在 NACK 时立即返回（Timeout 只约束等标志位，
 *   不是探测间隔）。旧版 at24c02_write 的 20 次探测之间无任何间隔，
 *   整轮在 <<1 ms 内耗尽 -> EEPROM_Save 误报失败。这个桩把"写周期忙"
 *   建模为"tick < write_busy_until 时探测/读一律 NACK"，
 *   HAL_Delay(ms) 推进 tick —— 于是：
 *     · 旧代码（无间隔）：探测轮询在 tick 不变的情况下耗尽 -> Save 失败（测试红）
 *     · 新代码（每次 HAL_Delay(1)）：tick 推进过忙窗 -> Save 成功（测试绿）
 *   这条用例（test_eeprom.c 的 write_cycle_busy_then_ack）是该修复的回归锁。
 *
 * 故障注入维度（对应"回读校验 + I2C 锁死恢复"两条新路径）：
 *   · corrupt_next_writes：接下来 N 次 Mem_Write 把存储内容按位取反
 *     -> 模拟"ACK 了但内容没写对"，EEPROM_Save 必须回读校验失败并走
 *        force_reinit + 重试路径。
 *   · always_nack_ready：**器件级**故障——地址探测永远 NACK（DeInit 复位
 *     外设也救不了）-> Save 在轮询耗尽 + 重试后返回 false。
 *   · periph_stuck：**外设级**锁死——IsDeviceReady 永远 NACK 但器件其实
 *     正常（Mem_Write 照常落盘）；DeInit 清除该状态 -> Save 第一次失败，
 *     force_reinit 重试后成功。
 *   · force_memwrite_fail / force_memread_fail：传输层直接失败。
 *   · force_init_fail：HAL_I2C_Init 失败 -> s_i2c_ready 不置位 -> Save 立即 false。
 */
#include "stm32f1xx_hal.h"
#include "eeprom.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- 假端口 / 假 I2C 句柄（eeprom.c 只透传地址，从不解引用内容） ---- */
GPIO_TypeDef s_fake_gpio_a, s_fake_gpio_b, s_fake_gpio_d, s_fake_gpio_e;
I2C_HandleTypeDef s_fake_i2c1;

/* ---- 可控时钟 ---- */
static uint32_t s_tick = 0;
uint32_t HAL_GetTick(void) { return s_tick; }
void     HAL_Delay(uint32_t ms) { s_tick += ms; }   /* 主机上"延迟"= 推进 tick */

static int s_clk_count = 0;
void HostStub_RecordClkEnable(void) { s_clk_count++; }

uint32_t HAL_RCC_GetHCLKFreq(void) { return 72000000U; }

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) {
    (void)port; (void)init;
}
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st) {
    (void)port; (void)pin; (void)st;
}

/* ---- 伪 AT24C02 ---- */
#define FAKE_EE_SIZE 256
static uint8_t  s_ee[FAKE_EE_SIZE];
static uint32_t s_write_busy_until = 0;   /* tick < 此值 = 内部写周期忙 */

/* 故障注入开关 */
static int s_force_init_fail     = 0;
static int s_force_memwrite_fail = 0;
static int s_force_memread_fail  = 0;
static int s_force_always_nack_ready = 0;    /* 器件级：探测永远 NACK，DeInit 救不了 */
static int s_periph_stuck        = 0;        /* 外设级：探测永远 NACK，DeInit 可清除 */
static int s_corrupt_next_writes = 0;        /* 接下来 N 次写把内容按位取反 */

/* 交互计数（断言"走了哪条路径"） */
static int s_init_count = 0;
static int s_deinit_count = 0;
static int s_memwrite_count = 0;
static int s_memread_count = 0;
static int s_ready_poll_count = 0;
static int s_iwdg_feed_count = 0;

HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c) {
    (void)hi2c;
    s_init_count++;
    return s_force_init_fail ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c) {
    (void)hi2c;
    s_deinit_count++;
    /* DeInit 清"外设锁死"：复位忙窗与 periph_stuck（真实外设复位后 BUSY 清除）；
     * always_nack（器件级故障）**不**清除——换外设也救不了不应答的器件。 */
    s_write_busy_until = 0;
    s_periph_stuck = 0;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize,
                                    uint8_t *pData, uint16_t Size, uint32_t Timeout) {
    (void)hi2c; (void)DevAddress; (void)MemAddSize; (void)Timeout;
    s_memwrite_count++;
    if (s_force_memwrite_fail) {
        return HAL_ERROR;
    }
    if (MemAddress + Size > FAKE_EE_SIZE) {
        return HAL_ERROR;
    }
    memcpy(&s_ee[MemAddress], pData, Size);
    if (s_corrupt_next_writes > 0) {
        for (uint16_t i = 0; i < Size; i++) {
            s_ee[MemAddress + i] ^= 0xFFU;      /* 模拟"写歪了" */
        }
        s_corrupt_next_writes--;
    }
    /* 页写入完成后进入内部写周期：探测/读在忙窗内一律 NACK */
    s_write_busy_until = s_tick + 5;           /* 5 ms 写周期 */
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize,
                                   uint8_t *pData, uint16_t Size, uint32_t Timeout) {
    (void)hi2c; (void)DevAddress; (void)MemAddSize; (void)Timeout;
    s_memread_count++;
    if (s_force_memread_fail) {
        return HAL_ERROR;
    }
    if (s_tick < s_write_busy_until) {
        return HAL_ERROR;                      /* 写周期内读也 NACK（真器件行为） */
    }
    if (MemAddress + Size > FAKE_EE_SIZE) {
        return HAL_ERROR;
    }
    memcpy(pData, &s_ee[MemAddress], Size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                        uint32_t Trials, uint32_t Timeout) {
    (void)hi2c; (void)DevAddress; (void)Trials; (void)Timeout;
    s_ready_poll_count++;
    if (s_force_always_nack_ready || s_periph_stuck) {
        return HAL_ERROR;
    }
    /* ★ 关键模型：忙窗内探测**立即** NACK（不等 Timeout）—— 这正是
     * HAL_I2C_IsDeviceReady 的真实语义，也是旧代码轮询间隔陷阱的根源。 */
    return (s_tick < s_write_busy_until) ? HAL_ERROR : HAL_OK;
}

/* ---- 测试注入 / 读取接口 ---- */
void HostEe_Reset(void) {
    memset(s_ee, 0xFF, FAKE_EE_SIZE);          /* 空片 = 全 0xFF */
    s_tick = 0;
    s_write_busy_until = 0;
    s_force_init_fail = 0;
    s_force_memwrite_fail = 0;
    s_force_memread_fail = 0;
    s_force_always_nack_ready = 0;
    s_periph_stuck = 0;
    s_corrupt_next_writes = 0;
    s_init_count = 0;
    s_deinit_count = 0;
    s_memwrite_count = 0;
    s_memread_count = 0;
    s_ready_poll_count = 0;
    s_iwdg_feed_count = 0;
}
void HostEe_SetInitFail(int on)          { s_force_init_fail = on; }
void HostEe_SetMemWriteFail(int on)      { s_force_memwrite_fail = on; }
void HostEe_SetMemReadFail(int on)       { s_force_memread_fail = on; }
void HostEe_SetAlwaysNackReady(int on)   { s_force_always_nack_ready = on; }
void HostEe_SetPeriphStuck(int on)       { s_periph_stuck = on; }
void HostEe_SetCorruptNextWrites(int n)  { s_corrupt_next_writes = n; }
int  HostEe_InitCount(void)              { return s_init_count; }
int  HostEe_DeInitCount(void)            { return s_deinit_count; }
int  HostEe_MemWriteCount(void)          { return s_memwrite_count; }
int  HostEe_MemReadCount(void)           { return s_memread_count; }
int  HostEe_ReadyPollCount(void)         { return s_ready_poll_count; }
int  HostEe_IwdgFeedCount(void)          { return s_iwdg_feed_count; }
const uint8_t *HostEe_Raw(void)          { return s_ee; }
void HostEe_Poke(uint8_t addr, uint8_t v) {
    s_ee[addr] = v;                          /* addr 为 uint8_t，必在 256 B 范围内 */
}

/* ---- 其它桩 ---- */
void Iwdg_Feed(void) { s_iwdg_feed_count++; }

static int s_dbg_enabled = 0;
static int s_dbg_lines = 0;
void Dbg_Printf(const char *fmt, ...) {
    s_dbg_lines++;
    if (!s_dbg_enabled) return;
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
void HostEe_SetDbg(int on) { s_dbg_enabled = on; }
int  HostEe_DbgLines(void) { return s_dbg_lines; }
