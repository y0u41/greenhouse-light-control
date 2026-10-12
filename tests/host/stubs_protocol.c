/*
 * 协议套件专用桩。
 *
 * 为什么不能复用 stubs.c：stubs.c 里定义了 Protocol_SendAck / Protocol_SendEvent
 * 来记录 control.c 的上行行为，而这两个函数**正是 protocol.c 自己实现的** ——
 * 一起链接会重复定义。所以协议套件需要一套更小的桩。
 */
#include "stm32f1xx_hal.h"
#include "protocol.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- 下行指令记录：断言"哪些帧真正被执行了" ---- */
#define CMD_MAX 32
typedef struct { char action[64]; int32_t value; } cmd_ev_t;
static cmd_ev_t s_cmds[CMD_MAX];
static int      s_cmd_n = 0;

void Control_OnCommand(const char *action, int32_t value) {
    if (s_cmd_n < CMD_MAX) {
        memset(&s_cmds[s_cmd_n], 0, sizeof(s_cmds[0]));
        if (action) strncpy(s_cmds[s_cmd_n].action, action, sizeof(s_cmds[0].action) - 1);
        s_cmds[s_cmd_n].value = value;
        s_cmd_n++;
    }
}
void HostP_ClearCmds(void) { s_cmd_n = 0; }
int  HostP_CmdCount(void) { return s_cmd_n; }
const char *HostP_CmdActionAt(int i) {
    return (i >= 0 && i < s_cmd_n) ? s_cmds[i].action : NULL;
}
int32_t HostP_CmdValueAt(int i) {
    return (i >= 0 && i < s_cmd_n) ? s_cmds[i].value : -99999;
}

/* ---- 上行出口记录：断言"发出了哪些帧、内容长什么样" ---- */
#define RAW_MAX 32
typedef struct { char text[512]; uint16_t len; } raw_ev_t;
static raw_ev_t s_raw[RAW_MAX];
static int      s_raw_n = 0;

void Esp8266_SendRaw(const char *data, uint16_t len) {
    if (s_raw_n < RAW_MAX) {
        uint16_t n = (len < sizeof(s_raw[0].text) - 1) ? len : (uint16_t)(sizeof(s_raw[0].text) - 1);
        memcpy(s_raw[s_raw_n].text, data, n);
        s_raw[s_raw_n].text[n] = '\0';
        s_raw[s_raw_n].len = len;
        s_raw_n++;
    }
}
void HostP_ClearRaw(void) { s_raw_n = 0; }
int  HostP_RawCount(void) { return s_raw_n; }
const char *HostP_RawAt(int i) {
    return (i >= 0 && i < s_raw_n) ? s_raw[i].text : NULL;
}

/* ---- 其它 ---- */
static int s_dbg = 0;
void Dbg_Printf(const char *fmt, ...) {
    if (!s_dbg) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void HostP_SetDbg(int on) { s_dbg = on; }

uint32_t HAL_GetTick(void) { return 0; }
uint32_t HAL_RCC_GetHCLKFreq(void) { return 72000000U; }

/* 协议套件不需要的模块接口（避免链接缺失） */
uint16_t Light_ReadRaw(uint8_t t) { (void)t; return 0; }
uint8_t  Light_IsValid(void) { return 1; }
void     LightSensor_Init(void) { }
bool     EEPROM_Save(uint16_t a, uint16_t b) { (void)a; (void)b; return true; }
void     Actuator_Init(void) { }
void     Actuator_Set(uint8_t on) { (void)on; }
void     Buzzer_Init(void) { }
void     Buzzer_Set(uint8_t on) { (void)on; }
void     Control_Init(void) { }
void     Control_Apply(uint16_t t, uint16_t h, uint8_t m) { (void)t; (void)h; (void)m; }
void     Control_Loop(uint16_t l) { (void)l; }
