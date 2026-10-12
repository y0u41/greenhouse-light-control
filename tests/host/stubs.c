/*
 * 主机测试桩：替代 HAL 与其它业务模块，使 control.c / protocol.c
 * 能在 PC 上**未经改动**地编译运行。
 *
 * 同时把"外部交互"记录下来（GPIO 写、发出的 ack/evt、EEPROM 写），
 * 这样测试断言的是**可观测行为**，而不是内部变量 —— 后者会让测试
 * 与实现耦合，重构即失效。
 */
#include "stm32f1xx_hal.h"
#include "control.h"
#include "protocol.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ============ 假端口 ============ */
GPIO_TypeDef s_fake_gpio_a, s_fake_gpio_b, s_fake_gpio_d, s_fake_gpio_e;

/* ============ 业务全局量（正常定义在 main.c）============ */
uint16_t g_adc  = 0;
uint16_t g_light = 0;
uint16_t g_th   = 2000;
uint16_t g_hys  = 150;
uint8_t  g_relay = 0;
uint8_t  g_buzz  = 0;
uint8_t  g_mode  = 0;

/* ============ 可控时钟（测试可推进）============ */
static uint32_t s_tick = 0;
uint32_t HAL_GetTick(void) { return s_tick; }
void     HostStub_SetTick(uint32_t t) { s_tick = t; }
void     HostStub_AdvanceTick(uint32_t d) { s_tick += d; }

static int s_clk_enable_count = 0;
void HostStub_RecordClkEnable(void) { s_clk_enable_count++; }
int  HostStub_ClkEnableCount(void) { return s_clk_enable_count; }

/* ============ GPIO 写记录 ============ */
#define GPIO_LOG_MAX 64
typedef struct { GPIO_TypeDef *port; uint16_t pin; GPIO_PinState st; } gpio_ev_t;
static gpio_ev_t s_gpio_log[GPIO_LOG_MAX];
static int       s_gpio_n = 0;

/* 引脚**当前电平**表：与"写事件日志"分开维护。
 * 为什么必须分开：像"死区内保持"这类用例**根本不产生写操作**，
 * 若只靠写日志推断电平，就会把"没有写"误判成"电平不对"。
 * 电平表只在显式 reset 时清空，写操作时更新，因此任何时刻都能读到
 * "这个引脚现在是什么电平"。 */
#define LEVEL_MAX 32
typedef struct { GPIO_TypeDef *port; uint16_t pin; int level; int used; } lvl_t;
static lvl_t s_lvl[LEVEL_MAX];

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) {
    (void)port; (void)init;                     /* 只关心"有没有配"，不关心细节 */
}
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st) {
    if (s_gpio_n < GPIO_LOG_MAX) {
        s_gpio_log[s_gpio_n].port = port;
        s_gpio_log[s_gpio_n].pin  = pin;
        s_gpio_log[s_gpio_n].st   = st;
        s_gpio_n++;
    }
    /* 同步更新电平表 */
    int slot = -1;
    for (int i = 0; i < LEVEL_MAX; i++) {
        if (s_lvl[i].used && s_lvl[i].port == port && s_lvl[i].pin == pin) { slot = i; break; }
        if (!s_lvl[i].used && slot < 0) slot = i;     /* 记住第一个空位备用 */
    }
    if (slot >= 0) {
        s_lvl[slot].used  = 1;
        s_lvl[slot].port  = port;
        s_lvl[slot].pin   = pin;
        s_lvl[slot].level = (int)st;
    }
}
/* 引脚当前电平；-1 表示从未被写过 */
int HostStub_PinLevel(GPIO_TypeDef *port, uint16_t pin) {
    for (int i = 0; i < LEVEL_MAX; i++) {
        if (s_lvl[i].used && s_lvl[i].port == port && s_lvl[i].pin == pin)
            return s_lvl[i].level;
    }
    return -1;
}
void HostStub_ClearPinLevels(void) {
    for (int i = 0; i < LEVEL_MAX; i++) s_lvl[i].used = 0;
}
void HostStub_ClearGpioLog(void) { s_gpio_n = 0; }
int  HostStub_GpioLogCount(void) { return s_gpio_n; }
/* 返回最后一次对指定引脚的写；-1 表示没有记录 */
int  HostStub_LastWrite(GPIO_TypeDef *port, uint16_t pin) {
    for (int i = s_gpio_n - 1; i >= 0; i--) {
        if (s_gpio_log[i].port == port && s_gpio_log[i].pin == pin)
            return (int)s_gpio_log[i].st;
    }
    return -1;
}
uint32_t HAL_RCC_GetHCLKFreq(void) { return 72000000U; }

/* ============ 协议上行记录 ============ */
typedef struct {
    char     kind[8];        /* "ack" / "evt" */
    char     action[64];
    char     result[16];
    uint8_t  node, relay, buzz, mode;
    uint16_t adc, light, th, hys;
} uplink_ev_t;
#define UPLINK_MAX 64
static uplink_ev_t s_up[UPLINK_MAX];
static int         s_up_n = 0;

void Protocol_SendAck(const char *action, const char *result, uint8_t node,
                      uint16_t adc, uint16_t light, uint16_t th, uint16_t hys,
                      uint8_t relay, uint8_t buzz, uint8_t mode) {
    if (s_up_n < UPLINK_MAX) {
        uplink_ev_t *e = &s_up[s_up_n++];
        memset(e, 0, sizeof(*e));
        strncpy(e->kind, "ack", sizeof(e->kind) - 1);
        if (action) strncpy(e->action, action, sizeof(e->action) - 1);
        if (result) strncpy(e->result, result, sizeof(e->result) - 1);
        e->node = node; e->adc = adc; e->light = light; e->th = th; e->hys = hys;
        e->relay = relay; e->buzz = buzz; e->mode = mode;
    }
}
void Protocol_SendEvent(const char *event, uint8_t node, uint16_t adc, uint16_t light) {
    if (s_up_n < UPLINK_MAX) {
        uplink_ev_t *e = &s_up[s_up_n++];
        memset(e, 0, sizeof(*e));
        strncpy(e->kind, "evt", sizeof(e->kind) - 1);
        if (event) strncpy(e->action, event, sizeof(e->action) - 1);
        strncpy(e->result, "-", sizeof(e->result) - 1);
        e->node = node; e->adc = adc; e->light = light;
    }
}
void HostStub_ClearUplink(void) { s_up_n = 0; }
int  HostStub_UplinkCount(void) { return s_up_n; }
const uplink_ev_t *HostStub_UplinkAt(int i) {
    return (i >= 0 && i < s_up_n) ? &s_up[i] : NULL;
}
/* 最近一条指定 kind 的事件；kind 传 NULL 表示不限 */
const uplink_ev_t *HostStub_LastUplink(const char *kind) {
    for (int i = s_up_n - 1; i >= 0; i--) {
        if (!kind || strcmp(s_up[i].kind, kind) == 0) return &s_up[i];
    }
    return NULL;
}

/* ============ EEPROM 桩 ============ */
static bool s_eeprom_ok = true;
static int  s_eeprom_calls = 0;
static uint16_t s_eeprom_last_th = 0, s_eeprom_last_hys = 0;

bool EEPROM_Save(uint16_t th, uint16_t hys) {
    s_eeprom_calls++;
    s_eeprom_last_th = th; s_eeprom_last_hys = hys;
    return s_eeprom_ok;
}
void HostStub_SetEepromOk(bool ok) { s_eeprom_ok = ok; }
int  HostStub_EepromCalls(void) { return s_eeprom_calls; }
void HostStub_ResetEepromCalls(void) { s_eeprom_calls = 0; }
void HostStub_GetEepromLast(uint16_t *th, uint16_t *hys) {
    if (th) *th = s_eeprom_last_th;
    if (hys) *hys = s_eeprom_last_hys;
}

/* ============ 光照有效性桩 ============ */
static uint8_t s_light_valid = 1;
uint8_t Light_IsValid(void) { return s_light_valid; }
void HostStub_SetLightValid(uint8_t v) { s_light_valid = v; }

/* ============ Dbg_Printf 桩：默认静音，避免刷屏 ============ */
static int s_dbg_enabled = 0;
static int s_dbg_lines = 0;
void Dbg_Printf(const char *fmt, ...) {
    s_dbg_lines++;
    if (!s_dbg_enabled) return;
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
void HostStub_SetDbg(int on) { s_dbg_enabled = on; }
int  HostStub_DbgLines(void) { return s_dbg_lines; }

/* ============ 未使用的模块接口（避免链接缺失）============ */
uint16_t Light_ReadRaw(uint8_t times) { (void)times; return g_adc; }
void     LightSensor_Init(void) { }
