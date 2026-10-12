/**
 * @file control.c
 * @brief 控制核心：执行器/蜂鸣器输出、回差状态机、手动优先、set_th 校验与掉电保存。
 *
 * 三条不变式（Tech_Design 附录 A.2 / §回差控制）：
 *  1) 阈值判断只建立在 light = 4095 - adc 上；
 *  2) 0 <= hys < th <= 4095，在 EEPROM 装载与 set_th 两处强制
 *     （否则 th - hys 无符号下溢，比较永假，继电器锁死无法释放）；
 *  3) 手动优先：relay_on/relay_off 切手动模式，auto 才回到自动逻辑。
 */

#include "control.h"
#include "protocol.h"
#include "eeprom.h"
#include "adc_light.h"     /* Light_IsValid()：采样无效时拒绝自动决策 */
#include "debug_uart.h"    /* Dbg_Printf：EEPROM 写盘失败要留下可见记录 */

/* ===================== 执行器抽象 ===================== */
/* 阶段一配置：板载 LED1（PE5）作等效负载（先验实验） */
#define ACTUATOR_GPIO_PORT      GPIOE
#define ACTUATOR_GPIO_PIN       GPIO_PIN_5
#define ACTUATOR_CLK_ENABLE()   do { __HAL_RCC_GPIOE_CLK_ENABLE(); } while (0)
#define ACTUATOR_NAME           "LED1(等效负载)"

/* 阶段二配置：借用继电器模块到位后，把上面 4 行整体换成下面 4 行（其余代码不动）
#define ACTUATOR_GPIO_PORT      GPIOD
#define ACTUATOR_GPIO_PIN       GPIO_PIN_11
#define ACTUATOR_CLK_ENABLE()   do { __HAL_RCC_GPIOD_CLK_ENABLE(); } while (0)
#define ACTUATOR_NAME           "Relay(PD11)"
*/

/* 0 = 低电平有效。LED1 与低电平触发继电器模块一致，两阶段切换无需改这一行 */
#define ACTUATOR_ACTIVE_LEVEL   0

/* 蜂鸣器：PB8，高电平响 */
#define BUZZER_GPIO_PORT        GPIOB
#define BUZZER_GPIO_PIN         GPIO_PIN_8

/* 报警测试脉冲截止时刻（HAL_GetTick 基准，0 = 无脉冲待处理） */
static uint32_t s_buzz_pulse_end = 0;

/* ===================== 输出层 ===================== */

void Actuator_Init(void)
{
    GPIO_InitTypeDef gi = {0};

    ACTUATOR_CLK_ENABLE();
    /* 先置"断开"再配 IO，避免上电瞬间误动作 */
    HAL_GPIO_WritePin(ACTUATOR_GPIO_PORT, ACTUATOR_GPIO_PIN,
                      (ACTUATOR_ACTIVE_LEVEL ? GPIO_PIN_RESET : GPIO_PIN_SET));
    gi.Pin   = ACTUATOR_GPIO_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ACTUATOR_GPIO_PORT, &gi);
}

void Actuator_Set(uint8_t on)
{
    GPIO_PinState s = on
        ? (ACTUATOR_ACTIVE_LEVEL ? GPIO_PIN_SET : GPIO_PIN_RESET)
        : (ACTUATOR_ACTIVE_LEVEL ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(ACTUATOR_GPIO_PORT, ACTUATOR_GPIO_PIN, s);
}

void Buzzer_Init(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(BUZZER_GPIO_PORT, BUZZER_GPIO_PIN, GPIO_PIN_RESET);  /* 上电必须为关 */
    gi.Pin   = BUZZER_GPIO_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BUZZER_GPIO_PORT, &gi);
}

void Buzzer_Set(uint8_t on)
{
    /* 高电平响 */
    HAL_GPIO_WritePin(BUZZER_GPIO_PORT, BUZZER_GPIO_PIN,
                      (on ? GPIO_PIN_SET : GPIO_PIN_RESET));
}

/* ===================== 报警声占空比（relay 与 buzz 解耦）=====================
 * 原实现 `g_buzz = g_relay` 使二者**恒等**，于是 MODE_AUTO 下只要光过强，
 * 执行器吸合的同时蜂鸣器会**连续长鸣数小时/整夜** —— 这不是"报警"，
 * 而且 `relay_on` 之后无法单独静音（alarm_test 到期也只是恢复成 g_relay）。
 * 参考工程用响/停占空比（app_config.h:111-112 的 APP_ALARM_TIME_MS /
 * APP_ALARM_GAP_MS 均为 10000），这里照抄该口径：响 10 s、停 10 s、循环。
 * "联动"保留为"报警期间蜂鸣器按节奏提示"，而不是"与执行器同寿命的常鸣"。 */
#define ALARM_BEEP_ON_MS     10000U   /* 响 10 s */
#define ALARM_BEEP_OFF_MS    10000U   /* 静默 10 s */

static uint32_t s_beep_phase_end = 0U;   /* 当前相位结束时刻（0 = 无报警） */
static uint8_t  s_beep_on        = 0U;   /* 当前相位：1=正在响 */

/**
 * @brief 输出统一出口：任何路径改输出都走这里，避免执行器/蜂鸣器状态漂移。
 * relay 与 buzz **解耦**：relay 表示执行器状态，buzz 由占空比相位驱动。
 */
static void Control_ApplyOutput(uint8_t on)
{
    g_relay = on;
    Actuator_Set(on);

    if (on != 0U) {
        /* 进入报警：蜂鸣器立刻开始响，并进入"响"相位 */
        s_beep_on        = 1U;
        s_beep_phase_end = HAL_GetTick() + ALARM_BEEP_ON_MS;
        g_buzz           = 1U;
        Buzzer_Set(1U);
    } else {
        /* 退出报警：立刻静音并清相位 */
        s_beep_on        = 0U;
        s_beep_phase_end = 0U;
        g_buzz           = 0U;
        Buzzer_Set(0U);
    }
}

/* ===================== 控制状态机 ===================== */

void Control_Init(void)
{
    Actuator_Init();
    Buzzer_Init();
    Control_ApplyOutput(0);            /* 上电执行器断开、蜂鸣器不响 */
}

void Control_Apply(uint16_t th, uint16_t hys, uint8_t mode)
{
    /* 装载期也强制不变式，防止坏值把状态机带进死区 */
    if (hys >= th || th > LIGHT_ADC_MAX) {
        th = TH_DEFAULT;
        hys = HYS_DEFAULT;
    }
    g_th = th;
    g_hys = hys;
    g_mode = (mode == MODE_MANUAL) ? MODE_MANUAL : MODE_AUTO;
}

/* 自动模式下的目标输出：回差在这里体现 */
static uint8_t Control_AutoTarget(uint16_t light)
{
    if (light > g_th)                       return 1;   /* 光过强 → 遮阳/报警 */
    if (light < (uint16_t)(g_th - g_hys))   return 0;   /* 回落到回差以下才释放 */
    return g_relay;                                     /* 死区：保持 */
}

void Control_Loop(uint16_t light)
{
    /* 报警测试脉冲到期：恢复到**占空比相位**（而不是直接 = g_relay，
     * 否则会把当前"静默"相位破坏掉，变成常鸣）。 */
    if (s_buzz_pulse_end != 0U && (int32_t)(HAL_GetTick() - s_buzz_pulse_end) >= 0) {
        s_buzz_pulse_end = 0U;
        g_buzz = (g_relay != 0U) ? s_beep_on : 0U;
        Buzzer_Set(g_buzz);
    }

    /* ★ 报警声占空比推进 ★ 放在模式判断**之前**，手动吸合时同样受保护。
     * 每 ALARM_BEEP_ON_MS 响 / ALARM_BEEP_OFF_MS 停 交替，避免长鸣。 */
    if (g_relay != 0U && s_beep_phase_end != 0U &&
        (int32_t)(HAL_GetTick() - s_beep_phase_end) >= 0) {
        s_beep_on        = (uint8_t)(s_beep_on == 0U ? 1U : 0U);
        s_beep_phase_end = HAL_GetTick() +
                           (s_beep_on != 0U ? ALARM_BEEP_ON_MS : ALARM_BEEP_OFF_MS);
        g_buzz = s_beep_on;
        Buzzer_Set(s_beep_on);
    }

    if (g_mode != MODE_AUTO) {
        return;                             /* 手动模式：自动逻辑完全不介入，不会回弹 */
    }

    /* ★ 采样无效时**不做任何自动决策**（维持当前输出）。
     * 理由：Light_ReadRaw 失败时返回的是"上次有效值"，虽然已经比原来的 0 安全得多，
     * 但连续失败（传感器脱线/ADC 异常）意味着当前光强**未知**，
     * 此时任何自动动作（尤其"光过强→吸合"）都是无依据的。
     * 宁可停在一个确定的安全状态，也不要用未知输入驱动执行器。 */
    if (!Light_IsValid()) {
        return;
    }

    uint8_t target = Control_AutoTarget(light);
    if (target != g_relay) {
        Control_ApplyOutput(target);
        Protocol_SendEvent(target ? "alarm" : "recover", NODE_ID, g_adc, light);
    }
}

void Control_OnCommand(const char *action, int32_t value)
{
    if (strcmp(action, "relay_on") == 0) {
        g_mode = MODE_MANUAL;
        Control_ApplyOutput(1);
        Protocol_SendAck("relay_on", "ok", NODE_ID, g_adc, g_light,
                         g_th, g_hys, g_relay, g_buzz, g_mode);
    } else if (strcmp(action, "relay_off") == 0) {
        g_mode = MODE_MANUAL;
        Control_ApplyOutput(0);
        Protocol_SendAck("relay_off", "ok", NODE_ID, g_adc, g_light,
                         g_th, g_hys, g_relay, g_buzz, g_mode);
    } else if (strcmp(action, "auto") == 0) {
        g_mode = MODE_AUTO;                 /* 下一轮 Control_Loop 按当前 light 重算 */
        Protocol_SendAck("auto", "ok", NODE_ID, g_adc, g_light,
                         g_th, g_hys, g_relay, g_buzz, g_mode);
    } else if (strcmp(action, "set_th") == 0) {
        if (value < 0 || value > (int32_t)LIGHT_ADC_MAX || (uint16_t)value <= g_hys) {
            Protocol_SendAck("set_th", "err", NODE_ID, g_adc, g_light,
                             g_th, g_hys, g_relay, g_buzz, g_mode);
            return;                         /* 非法值拒绝并回 err，不静默截断 */
        }
        /* ★ 如实回报落盘结果（原来丢弃返回值后无条件回 "ok"）。
         * EEPROM_Save 会因 I2C 写失败或 20 次 ACK 轮询全败而返回 false，
         * 而 24C02 不在位是这类板子很常见的状态。原实现让上位机以为已保存，
         * 重启后却悄悄回到旧值 —— 谎报比明确拒绝更糟。
         * 这里**回滚 g_th**，让 "err" 的语义明确且唯一：**指令未生效**。
         * （若保留内存新值却回 err，上位机就会面对"报错但状态其实变了"的二义。） */
        uint16_t old_th = g_th;
        g_th = (uint16_t)value;
        if (!EEPROM_Save(g_th, g_hys)) {
            g_th = old_th;                 /* 回滚：err = 未生效，不留半生效状态 */
            Dbg_Printf("[cfg] EEPROM_Save FAILED for th=%u -> rolled back to %u\r\n",
                       (unsigned)value, (unsigned)g_th);
            Protocol_SendAck("set_th", "err", NODE_ID, g_adc, g_light,
                             g_th, g_hys, g_relay, g_buzz, g_mode);
            return;
        }
        Protocol_SendAck("set_th", "ok", NODE_ID, g_adc, g_light,
                         g_th, g_hys, g_relay, g_buzz, g_mode);
    } else if (strcmp(action, "get_state") == 0) {
        Protocol_SendAck("get_state", "ok", NODE_ID, g_adc, g_light,
                         g_th, g_hys, g_relay, g_buzz, g_mode);   /* ack 已携带完整状态 */
    } else if (strcmp(action, "alarm_test") == 0) {
        /* 报警测试：蜂鸣器脉冲 1 s，不改模式与执行器；到期由 Control_Loop 恢复联动 */
        g_buzz = 1;
        Buzzer_Set(1);
        s_buzz_pulse_end = HAL_GetTick() + 1000U;
        Protocol_SendAck("alarm_test", "ok", NODE_ID, g_adc, g_light,
                         g_th, g_hys, g_relay, g_buzz, g_mode);
    } else {
        Protocol_SendAck(action, "err", NODE_ID, g_adc, g_light,
                         g_th, g_hys, g_relay, g_buzz, g_mode);   /* 未知动作一律拒绝 */
    }
}

/* ===================== 状态读取 ===================== */

uint16_t Control_GetTh(void)   { return g_th; }
uint16_t Control_GetHys(void)  { return g_hys; }
uint8_t  Control_GetMode(void) { return g_mode; }
uint8_t  Control_GetRelay(void){ return g_relay; }
uint8_t  Control_GetBuzz(void) { return g_buzz; }
