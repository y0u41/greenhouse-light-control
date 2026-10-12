/*
 * control.c 行为测试（主机端，无需硬件）
 *
 * 设计原则：
 *  1) 被测对象是**未经改动的真实 control.c** —— 通过最小 HAL 垫片在 PC 上编译，
 *     不是复制一份逻辑来测（复制品会随实现漂移而失去意义）。
 *  2) 断言**可观测行为**：执行器/蜂鸣器的实际 GPIO 电平 + 发出的 ack/evt。
 *     不断言内部静态变量，否则测试与实现耦合，一重构就红。
 *  3) 表驱动：边界用例写成数据表，新增边界只加一行。
 *
 * 覆盖 P0-2 要求：
 *  - 回差状态机边界三态（>th / <th-hys / 死区保持）
 *  - 手动优先
 *  - set_th 越界拒绝（表驱动）
 *  - EEPROM 写盘失败回滚
 *  - 采样无效时拒绝自动决策
 *  - 报警声占空比（relay 与 buzz 解耦）
 */

#include "stm32f1xx_hal.h"
#include "control.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* ---- 桩接口 ---- */
void HostStub_SetTick(uint32_t t);
void HostStub_AdvanceTick(uint32_t d);
void HostStub_ClearGpioLog(void);
void HostStub_ClearPinLevels(void);
int  HostStub_PinLevel(GPIO_TypeDef *port, uint16_t pin);
void HostStub_ClearUplink(void);
int  HostStub_UplinkCount(void);
void HostStub_SetEepromOk(bool ok);
int  HostStub_EepromCalls(void);
void HostStub_ResetEepromCalls(void);
void HostStub_GetEepromLast(uint16_t *th, uint16_t *hys);
void HostStub_SetLightValid(uint8_t v);
void HostStub_SetDbg(int on);
typedef struct {
    char kind[8]; char action[64]; char result[16];
    uint8_t node, relay, buzz, mode; uint16_t adc, light, th, hys;
} uplink_ev_t;
const uplink_ev_t *HostStub_LastUplink(const char *kind);

/* ---- 断言框架 ---- */
static int s_pass = 0, s_fail = 0;
static const char *s_group = "";

#define CHECK(cond, fmt, ...) do {                                        \
    if (cond) { s_pass++; }                                               \
    else {                                                                \
        s_fail++;                                                         \
        printf("  [FAIL] %s :: " fmt "\n", s_group, ##__VA_ARGS__);       \
    }                                                                     \
} while (0)

static void group(const char *name) {
    s_group = name;
    printf("\n== %s ==\n", name);
}

/* 执行器：ACTUATOR_ACTIVE_LEVEL = 0 -> on=1 写 GPIO_PIN_RESET(0) */
#define ACT_LEVEL(on)  ((on) ? GPIO_PIN_RESET : GPIO_PIN_SET)
/* 蜂鸣器：高电平响 */
#define BUZ_LEVEL(on)  ((on) ? GPIO_PIN_SET : GPIO_PIN_RESET)

static void actuator_pin(GPIO_TypeDef **port, uint16_t *pin) {
    /* control.c 里阶段一为 GPIOE/PE5；阶段二切到 GPIOD/PD11 时这里要同步 */
    *port = GPIOE;
    *pin  = GPIO_PIN_5;
}

static int relay_hw_level(void) {
    GPIO_TypeDef *p; uint16_t pin;
    actuator_pin(&p, &pin);
    /* 用"引脚当前电平"而非"最后一次写事件"：
     * "死区内保持"这类用例根本不产生写操作，用写日志会读不到电平。 */
    return HostStub_PinLevel(p, pin);
}
static int buzz_hw_level(void) { return HostStub_PinLevel(GPIOB, GPIO_PIN_8); }

/* 每个用例前重置到已知状态 */
static void reset_to(uint16_t th, uint16_t hys, uint8_t mode,
                     uint8_t relay_now, uint32_t tick) {
    HostStub_SetTick(tick);
    HostStub_ClearGpioLog();
    HostStub_ClearPinLevels();
    HostStub_ClearUplink();
    HostStub_ResetEepromCalls();
    g_adc = 0; g_light = 0; g_relay = 0; g_buzz = 0;
    g_th = th; g_hys = hys; g_mode = mode;
    HostStub_SetLightValid(1);
    HostStub_SetEepromOk(true);
    /* 通过真实接口把输出置到 relay_now，建立前态 */
    Control_Init();
    if (relay_now) {
        Control_OnCommand("relay_on", -1);
        /* ★ relay_on 会把 g_mode 置为 MANUAL —— 那是它的**正确行为**，
         * 但本函数是"建立前态"的夹具，请求的模式是参数 mode。
         * 若不恢复，所有 AUTO 用例都会因"手动优先"提前返回而**假失败**
         * （第一版正是栽在这里：14 个失败里有一半是它造成的）。
         * 夹具直接写 g_mode 建立状态，不参与任何断言，属合法操作。 */
        g_mode = mode;
    }
    HostStub_ClearGpioLog();
    HostStub_ClearUplink();
}

/* ============================================================
 * 1. 回差状态机：边界三态（表驱动）
 *    th=2000, hys=150  ->  上界 2000, 下界 1850
 * ============================================================ */
typedef struct {
    uint16_t light;
    uint8_t  relay_before;
    uint8_t  relay_after;
    const char *expect_evt;   /* NULL = 不应发 evt */
    const char *why;
} hyst_case_t;

static void test_hysteresis_boundaries(void) {
    group("回差状态机：边界三态");

    static const hyst_case_t cases[] = {
        /* light, before, after, evt,      说明 */
        { 4095, 0, 1, "alarm",   "满量程：> th -> 吸合" },
        { 2001, 0, 1, "alarm",   "上界+1：> th -> 吸合（边界外）" },
        { 2000, 0, 0, NULL,      "上界==th：不是 > th，且不 < 下界 -> 保持" },
        { 2000, 1, 1, NULL,      "上界==th 且已吸合 -> 保持（不抖）" },
        { 1851, 0, 0, NULL,      "死区内（上界下方） -> 保持 0" },
        { 1851, 1, 1, NULL,      "死区内（上界下方）且已吸合 -> 保持 1（回差作用）" },
        { 1850, 1, 1, NULL,      "下界==th-hys：不是 < 下界 -> 保持 1" },
        { 1849, 1, 0, "recover", "下界-1：< th-hys -> 释放（边界外）" },
        { 1849, 0, 0, NULL,      "下界-1 且已释放 -> 保持 0（不重复发事件）" },
        { 0,    1, 0, "recover", "最小光强 -> 释放" },
    };

    const int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        const hyst_case_t *c = &cases[i];
        reset_to(2000, 150, MODE_AUTO, c->relay_before, 1000);

        g_light = c->light;
        Control_Loop(c->light);

        CHECK(g_relay == c->relay_after,
              "light=%u before=%u: relay 期望 %u 实际 %u（%s）",
              c->light, c->relay_before, c->relay_after, g_relay, c->why);
        CHECK(relay_hw_level() == ACT_LEVEL(c->relay_after),
              "light=%u: 执行器硬件电平与状态不一致（%s）",
              c->light, c->why);

        const uplink_ev_t *e = HostStub_LastUplink("evt");
        if (c->expect_evt) {
            CHECK(e != NULL, "light=%u: 期望发出 evt %s，但没有（%s）",
                  c->light, c->expect_evt, c->why);
            if (e) {
                CHECK(strcmp(e->action, c->expect_evt) == 0,
                      "light=%u: evt 期望 %s 实际 %s",
                      c->light, c->expect_evt, e->action);
            }
        } else {
            CHECK(e == NULL, "light=%u: 不应发 evt，却发了 %s（%s）",
                  c->light, e ? e->action : "?", c->why);
        }
    }
    printf("  用例 %d 条\n", n);
}

/* ============================================================
 * 2. 回差的核心价值：死区内不振荡
 * ============================================================ */
static void test_no_oscillation_in_deadband(void) {
    group("回差：死区内往复扫描不产生动作");

    reset_to(2000, 150, MODE_AUTO, 0, 5000);
    g_light = 2500; Control_Loop(2500);           /* 越过上界 -> 吸合 */
    CHECK(g_relay == 1, "越上界后应吸合");

    HostStub_ClearUplink();
    /* 在死区内上下扫 20 次，不得有任何继电器动作或事件 */
    for (int k = 0; k < 20; k++) {
        uint16_t l = (k % 2) ? 1999 : 1851;
        Control_Loop(l);
        CHECK(g_relay == 1, "死区内 light=%u 不应释放", l);
    }
    CHECK(HostStub_LastUplink("evt") == NULL,
          "死区内往复扫描不应产生任何 evt（回差的意义所在）");
}

/* ============================================================
 * 3. 手动优先
 * ============================================================ */
static void test_manual_priority(void) {
    group("手动优先：MANUAL 下自动逻辑不介入");

    reset_to(2000, 150, MODE_MANUAL, 0, 1000);
    Control_OnCommand("relay_on", -1);            /* -> MANUAL + 吸合 */
    CHECK(g_mode == MODE_MANUAL, "relay_on 后应为 MANUAL");
    CHECK(g_relay == 1, "relay_on 后应吸合");

    HostStub_ClearUplink();
    /* 极端光强：若自动逻辑介入会被释放；手动模式下必须保持 */
    Control_Loop(0);
    CHECK(g_relay == 1, "MANUAL 下 light=0 不应释放（手动优先）");
    Control_Loop(4095);
    CHECK(g_relay == 1, "MANUAL 下 light=4095 应保持吸合");
    CHECK(HostStub_LastUplink("evt") == NULL,
          "MANUAL 下不应产生 alarm/recover 事件");

    /* 切回 AUTO 后自动逻辑应立即接管 */
    Control_OnCommand("auto", -1);
    CHECK(g_mode == MODE_AUTO, "auto 后应为 AUTO");
    Control_Loop(0);
    CHECK(g_relay == 0, "切回 AUTO 后 light=0 应释放");
}

/* ============================================================
 * 4. set_th 越界拒绝（表驱动）
 * ============================================================ */
typedef struct {
    int32_t  value;
    uint16_t hys;
    uint16_t th_before;
    bool     expect_ok;      /* true = 应回 ok 且生效 */
    const char *why;
} setth_case_t;

static void test_set_th_table(void) {
    group("set_th 越界拒绝（表驱动）");

    static const setth_case_t cases[] = {
        { -1,    150, 2000, false, "负值拒绝" },
        { 4096,  150, 2000, false, "超过 LIGHT_ADC_MAX 拒绝" },
        { 99999, 150, 2000, false, "远超量程拒绝" },
        { 150,   150, 2000, false, "== hys 拒绝（不变式要求 hys < th）" },
        { 100,   150, 2000, false, "< hys 拒绝" },
        { 0,     150, 2000, false, "0 拒绝" },
        { 151,   150, 2000, true,  "== hys+1 接受（下边界内）" },
        { 4095,  150, 2000, true,  "== 量程上限接受" },
        { 2001,  150, 2000, true,  "常规值接受" },
    };

    const int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        const setth_case_t *c = &cases[i];
        reset_to(c->th_before, c->hys, MODE_AUTO, 0, 1000);

        Control_OnCommand("set_th", c->value);

        const uplink_ev_t *a = HostStub_LastUplink("ack");
        CHECK(a != NULL, "value=%d: 应回 ack", (int)c->value);
        if (!a) continue;

        CHECK(strcmp(a->action, "set_th") == 0, "ack action 应为 set_th");
        CHECK(strcmp(a->result, c->expect_ok ? "ok" : "err") == 0,
              "value=%d hys=%u: 期望 result=%s 实际 %s（%s）",
              (int)c->value, c->hys, c->expect_ok ? "ok" : "err", a->result, c->why);

        if (c->expect_ok) {
            CHECK(g_th == (uint16_t)c->value,
                  "value=%d: g_th 应更新为 %d，实际 %u",
                  (int)c->value, (int)c->value, g_th);
            CHECK(HostStub_EepromCalls() == 1, "value=%d: 应写一次 EEPROM", (int)c->value);
        } else {
            CHECK(g_th == c->th_before,
                  "value=%d: 拒绝时 g_th 必须保持 %u，实际 %u（%s）",
                  (int)c->value, c->th_before, g_th, c->why);
        }
    }
    printf("  用例 %d 条\n", n);
}

/* ============================================================
 * 5. EEPROM 写盘失败必须回滚（err 语义 = 指令未生效）
 * ============================================================ */
static void test_eeprom_failure_rollback(void) {
    group("EEPROM 写盘失败：回 err 且回滚");

    reset_to(2000, 150, MODE_AUTO, 0, 1000);
    HostStub_SetEepromOk(false);
    Control_OnCommand("set_th", 3000);

    const uplink_ev_t *a = HostStub_LastUplink("ack");
    CHECK(a != NULL && strcmp(a->result, "err") == 0,
          "写盘失败应回 err（不能谎报 ok）");
    CHECK(g_th == 2000,
          "写盘失败必须回滚 g_th 到 2000，实际 %u（err 语义 = 未生效）", g_th);

    /* ack 里携带的 th 也应是回滚后的值，否则上位机会显示一个并未生效的阈值 */
    if (a) {
        CHECK(a->th == 2000, "err ack 里的 th 应为回滚值 2000，实际 %u", a->th);
    }
}

/* ============================================================
 * 6. 采样无效时拒绝自动决策
 * ============================================================ */
static void test_invalid_sample_gate(void) {
    group("采样无效：拒绝自动决策");

    reset_to(2000, 150, MODE_AUTO, 0, 1000);
    HostStub_SetLightValid(0);
    g_light = 4095;                     /* 数值看似"最强光"，但不可信 */
    Control_Loop(4095);
    CHECK(g_relay == 0, "采样无效时不应吸合（不能拿未知输入驱动执行器）");
    CHECK(HostStub_LastUplink("evt") == NULL, "采样无效时不应上报 alarm");

    /* 有效后正常动作 */
    HostStub_SetLightValid(1);
    Control_Loop(4095);
    CHECK(g_relay == 1, "恢复有效后应正常吸合");
}

/* ============================================================
 * 7. 报警声占空比（relay 与 buzz 解耦）
 * ============================================================ */
static void test_alarm_beep_duty_cycle(void) {
    group("报警声占空比：不常鸣");

    reset_to(2000, 150, MODE_AUTO, 0, 1000);
    Control_Loop(4095);                          /* 进入报警 */
    CHECK(g_relay == 1, "应吸合");
    CHECK(g_buzz == 1, "刚进入报警应立即开始响");
    CHECK(buzz_hw_level() == BUZ_LEVEL(1), "蜂鸣器硬件应为响");

    /* 推进到"响"相位结束：应转为静默，但继电器保持吸合 */
    HostStub_AdvanceTick(10000);
    Control_Loop(4095);
    CHECK(g_relay == 1, "占空比切换时继电器必须保持吸合");
    CHECK(g_buzz == 0, "10 s 后应转为静默（这就是不常鸣的关键）");
    CHECK(buzz_hw_level() == BUZ_LEVEL(0), "蜂鸣器硬件应为静默");

    /* 再推进 10 s：应再次响 */
    HostStub_AdvanceTick(10000);
    Control_Loop(4095);
    CHECK(g_buzz == 1, "再 10 s 后应再次响（响/停循环）");

    /* 释放后必须立刻静默 */
    Control_Loop(0);
    CHECK(g_relay == 0, "光弱应释放");
    CHECK(g_buzz == 0, "释放后蜂鸣器必须立刻静默");
    CHECK(buzz_hw_level() == BUZ_LEVEL(0), "释放后蜂鸣器硬件应为静默");
}

/* ============================================================
 * 8. 其它指令
 * ============================================================ */
static void test_other_commands(void) {
    group("其它指令：get_state / alarm_test / 未知动作");

    reset_to(2000, 150, MODE_AUTO, 1, 1000);

    HostStub_ClearUplink();
    Control_OnCommand("get_state", -1);
    const uplink_ev_t *a = HostStub_LastUplink("ack");
    CHECK(a && strcmp(a->action, "get_state") == 0 && strcmp(a->result, "ok") == 0,
          "get_state 应回 ok");
    CHECK(a && a->relay == 1, "get_state 的 ack 应携带当前 relay");

    HostStub_ClearUplink();
    Control_OnCommand("alarm_test", -1);
    a = HostStub_LastUplink("ack");
    CHECK(a && strcmp(a->action, "alarm_test") == 0 && strcmp(a->result, "ok") == 0,
          "alarm_test 应回 ok");
    CHECK(g_buzz == 1, "alarm_test 应立刻响");

    /* 脉冲到期后应回到占空比相位，而不是无脑 = g_relay（否则破坏"静默"相位） */
    HostStub_AdvanceTick(1000);
    Control_Loop(2000);
    CHECK(g_buzz == 1, "测试脉冲刚到期时，若正处于响相位则继续响");

    HostStub_ClearUplink();
    Control_OnCommand("no_such_action", -1);
    a = HostStub_LastUplink("ack");
    CHECK(a && strcmp(a->result, "err") == 0, "未知动作应回 err");
    CHECK(a && strcmp(a->action, "no_such_action") == 0, "ack 应回显 action 名");
}

/* ============================================================
 * 9. Control_Apply 的坏值兜底
 * ============================================================ */
static void test_apply_sanitizes_bad_values(void) {
    group("Control_Apply：坏值兜底");

    reset_to(2000, 150, MODE_AUTO, 0, 1000);

    Control_Apply(100, 200, MODE_AUTO);           /* hys >= th，非法 */
    CHECK(g_th == TH_DEFAULT && g_hys == HYS_DEFAULT,
          "hys>=th 应回退默认值，实际 th=%u hys=%u", g_th, g_hys);

    Control_Apply(5000, 150, MODE_AUTO);          /* th 超量程 */
    CHECK(g_th == TH_DEFAULT && g_hys == HYS_DEFAULT,
          "th 超量程应回退默认值，实际 th=%u", g_th);

    Control_Apply(3000, 100, MODE_MANUAL);
    CHECK(g_th == 3000 && g_hys == 100 && g_mode == MODE_MANUAL,
          "合法值应原样装载");

    Control_Apply(3000, 100, 0x7F);               /* 非法 mode */
    CHECK(g_mode == MODE_AUTO, "非法 mode 应归一为 AUTO，实际 %u", g_mode);
}

/* ============================================================ */
int main(void) {
    HostStub_SetDbg(0);
    printf("control.c 行为测试（被测对象：未改动的真实 control.c）\n");

    test_hysteresis_boundaries();
    test_no_oscillation_in_deadband();
    test_manual_priority();
    test_set_th_table();
    test_eeprom_failure_rollback();
    test_invalid_sample_gate();
    test_alarm_beep_duty_cycle();
    test_other_commands();
    test_apply_sanitizes_bad_values();

    printf("\n================================\n");
    printf("PASS %d   FAIL %d\n", s_pass, s_fail);
    printf("================================\n");
    return s_fail == 0 ? 0 : 1;
}
