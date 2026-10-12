/**
 * @file main.c
 * @brief 主流程：初始化、1 s 周期采集/上报、事件调度、喂狗。
 *
 * 初始化顺序与主循环节拍严格按 Tech_Design §固件设计 落地：
 *   HAL_Init → 时钟(72 MHz, ADCCLK 12 MHz) → IWDG(≈6.4 s)
 *   → LED0 → LCD → 光敏 → 执行器+蜂鸣器(安全态) → EEPROM 装载 → ESP8266 → 自动模式
 *   主循环：喂狗 → 1 s 采样/上报 + 心跳 → ESP8266 轮询 → 取帧执行 → 控制闭环
 *
 * 说明：定时节拍用 HAL_GetTick() 在主循环里比较实现，而非额外 TIM 中断。
 *      理由：少配置一个外设、杜绝"标志位被漏清"类问题，且 1 s 周期 +
 *      非阻塞 AT 状态机下抖动远小于 ADC 变化速度；需严格中断驱动时可改为 TIM2。
 *
 * ★ 本文件只留代码 + 指针 ★
 *   历史上这里的每一处"为什么"都曾写成大段叙事（半主机 bkpt、IWDG 12 位重装值、
 *   跨复位存活、优先级分组、启动顺序、复位源取证……）。那些内容已**原意保留**地
 *   整理进 `docs/lessons.md`，本文件只留 ≤3 行摘要 + 锚点，便于一眼读懂主流程。
 *   速查条目在 `AGENTS.md`「已知陷阱」，完整来龙去脉 + 证据 + 复现方法在 `docs/lessons.md`。
 */

#include "main.h"
#include "adc_light.h"
#include "control.h"
#include "protocol.h"
#include "eeprom.h"
#include "esp8266.h"
#include "lcd_show.h"
#include "debug_uart.h"
#include "delay.h"

/* WiFi / 上位机地址：优先 wifi_config.h（不入仓），缺失时用占位值，保证可编译 */
#if defined(__has_include)
#  if __has_include("wifi_config.h")
#    include "wifi_config.h"
#    define HAVE_WIFI_CONFIG 1
#  endif
#endif
#ifndef HAVE_WIFI_CONFIG
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASSWORD   "YOUR_PASSWORD"
#define TCP_SERVER_IP   "127.0.0.1"
#define TCP_SERVER_PORT 9092
#endif

/* ===================== 全局状态定义 ===================== */
uint16_t g_adc;
uint16_t g_light;
uint16_t g_th;
uint16_t g_hys;
uint8_t  g_relay;
uint8_t  g_buzz;
uint8_t  g_mode;

/* ===================== 时钟 / 看门狗 ===================== */

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef ro = {0};
    RCC_ClkInitTypeDef rc = {0};
    RCC_PeriphCLKInitTypeDef pc = {0};

    ro.OscillatorType  = RCC_OSCILLATORTYPE_HSE;
    ro.HSEState        = RCC_HSE_ON;
    ro.HSEPredivValue  = RCC_HSE_PREDIV_DIV1;
    ro.HSIState        = RCC_HSI_ON;
    ro.PLL.PLLState    = RCC_PLL_ON;
    ro.PLL.PLLSource   = RCC_PLLSOURCE_HSE;
    ro.PLL.PLLMUL      = RCC_PLL_MUL9;              /* 8 MHz × 9 = 72 MHz */
    if (HAL_RCC_OscConfig(&ro) != HAL_OK) {
        Error_Handler();
    }

    rc.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                      | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    rc.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    rc.AHBCLKDivider  = RCC_SYSCLK_DIV1;           /* HCLK = 72 MHz */
    rc.APB1CLKDivider = RCC_HCLK_DIV2;             /* PCLK1 = 36 MHz */
    rc.APB2CLKDivider = RCC_HCLK_DIV1;             /* PCLK2 = 72 MHz */
    if (HAL_RCC_ClockConfig(&rc, FLASH_LATENCY_2) != HAL_OK) {
        Error_Handler();
    }

    /* ADCCLK = PCLK2 / 6 = 12 MHz（Tech_Design 附录 B） */
    pc.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    pc.AdcClockSelection    = RCC_ADCPCLK2_DIV6;
    if (HAL_RCCEx_PeriphCLKConfig(&pc) != HAL_OK) {
        Error_Handler();
    }
}

/* IWDG 句柄：**必须保留**，HAL_IWDG_Init 需要它。
 * 但喂狗用裸写 IWDG->KR（见 Iwdg_Feed），因此即使 HAL_IWDG_Init 失败
 * （句柄里的 Instance 已赋值，不存在解引用 NULL 的问题）也不会崩。 */
static IWDG_HandleTypeDef s_hiwdg;

#ifndef DBGMCU_CR_DBG_IWDG_STOP
#define DBGMCU_CR_DBG_IWDG_STOP     0x00000100U
#endif

/* IWDG：LSI ≈40 kHz ÷ 64 = 625 Hz，重装 4000 → 溢出 ≈6.4 s
 * 返回状态供调用者判断，**失败绝不致命**（狗一旦启动就无法关闭；此处 Error_Handler
 * 会变成"死循环 + 狗没跑起来"，板子毫无反应且连复位都没有）。详见 docs/lessons.md#iwdg */
static HAL_StatusTypeDef MX_IWDG_Init(void)
{
    /* 初始化**必须**用 HAL_IWDG_Init：内部会轮询 IWDG_SR 的 PVU/RVU，而裸写序列会
     * 丢失这段等待、令重装值落回 POR 默认 4095。详见 docs/lessons.md#iwdg-sr-poll */
    DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;      /* 调试暂停时冻结 IWDG */

    s_hiwdg.Instance       = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_64; /* 64 分频 -> 625 Hz */
    /* 重装值必须 ≤ 4095（IWDG_RLR 只有 12 位，超了被静默截断）。
     * 详见 docs/lessons.md#iwdg-reload-12bit */
    s_hiwdg.Init.Reload    = 4000U;             /* 12 位上限内，实际约 6.4 s */
    return HAL_IWDG_Init(&s_hiwdg);             /* 内部会轮询 IWDG_SR */
}

void Iwdg_Feed(void)
{
    /* 直接写键值喂狗，**绝不要改回 HAL_IWDG_Refresh(&s_hiwdg)**：句柄 Instance 为空时
     * 等于往地址 0 写 0xAAAA（实测 HardFault 现场）。详见 docs/lessons.md#iwdg-refresh-null */
    IWDG->KR = 0xAAAAU;
}

/* ===================== LED0 运行心跳（PB5，低电平点亮） ===================== */

static void Led0_Init(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);   /* 置高 = 灭 */
    gi.Pin   = GPIO_PIN_5;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gi);
}

static void Led0_Toggle(void)
{
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_5);
}

/* 不依赖 tick 的粗忙等（约 0.1~0.2 s @72 MHz）。用 tick 反而危险：
 * 若 tick 本身有问题，延时函数会死等，闪灯码也就打不出来。 */
static void Led0_BusyWait(void)
{
    volatile uint32_t i;
    for (i = 0; i < 2000000U; i++) {
        __NOP();
    }
}

/* ============ 启动进度码：用红灯闪 n 次报"走到第几步" ============
 * 判读（1=HAL_Init 2=IWDG 3=时钟 4=LCD 5=采集/控制 6=EEPROM 7=通信）与恢复方法
 * 见 docs/lessons.md#boot-progress。当前只在启动末尾调用一次 Led0_BlinkMark(2)。 */
static void Led0_BlinkMark(uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET);   /* 亮 */
        Led0_BusyWait();
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);     /* 灭 */
        Led0_BusyWait();
    }
    /* 组间多留空，便于数清次数 */
    Led0_BusyWait();
    Led0_BusyWait();
}

/* ===================== 主流程 ===================== */

int main(void)
{
    uint16_t th  = TH_DEFAULT;
    uint16_t hys = HYS_DEFAULT;
    uint32_t last_tick;
    uint8_t  hb_cnt = 0;
    static char cmd_buf[PROTOCOL_FRAME_MAX];

    /* ---- 零依赖启动标志：在任何 HAL 调用之前，用纯寄存器闪 PB5 三下 ----
     * 目的：把"根本没进入 main()"与"进入 main 后在 HAL 里挂掉"分开。
     * 只用 RCC/GPIO 寄存器，不依赖 HAL、SysTick、中断、时钟配置，因此只要
     * 启动文件把控制权交给了 main()，这三下就一定闪。 */
    {
        volatile uint32_t d;
        uint8_t k;
        RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;          /* GPIOB 时钟 */
        GPIOB->CRL &= ~(0xFU << 20);                 /* PB5 推挽输出 */
        GPIOB->CRL |=  (0x3U << 20);
        for (k = 0; k < 3U; k++) {
            GPIOB->BRR  = GPIO_PIN_5;                /* 低 = 亮（低电平点亮） */
            for (d = 0; d < 500000U; d++) { __NOP(); }
            GPIOB->BSRR = GPIO_PIN_5;                /* 高 = 灭 */
            for (d = 0; d < 500000U; d++) { __NOP(); }
        }
    }

    /* 启动顺序保持最朴素的三步：HAL_Init → SystemClock_Config → 外设初始化。
     * 别在此处加"改进"（手工赋值 SystemCoreClock / 提前开狗 / 提前开调试串口）。
     * 详见 docs/lessons.md#boot-order */
    HAL_Init();
    SystemClock_Config();

    /* 中断优先级分组：**已回退，勿在此处恢复**（放在 HAL_Init 之后会让 SysTick 已写入的
     * IPR 被重新解释，实测直接导致复位循环）。详见 docs/lessons.md#nvic-grouping */

    /* 必须在时钟配好之后调用：BSP 的 lcd_init() 内部用 delay_ms(50)/(5) 做上电与
     * 复位时序，若 g_fac_us 仍为 0 则这些延时全部失效 -> LCD 初始化不成功 -> 黑屏。
     * 本工程用的是 HAL 友好版 delay.c（只记录倍乘数、不接管 SysTick），
     * 因此 SysTick 归 HAL 独占，HAL_GetTick/HAL_Delay 与主循环节拍都不受影响。 */
    delay_init(72);

    /* LED0 只保留**一次短的"启动完成"提示**（末尾 2 闪）：原先进度码 3~7 共 25 次闪烁使
     * 开机超过 25 秒，严重拖慢每轮验证与演示，故按要求删除。详见
     * docs/lessons.md#boot-progress（含进度码判读表与恢复方法）。 */
    Led0_Init();
#if DEBUG_UART
    Dbg_Init();
    Dbg_Printf("[boot] clock ok, node=%u\r\n", (unsigned)NODE_ID);
    /* 复位源取证：RCC_CSR 标志粘滞（系统复位不清），故本次读到的 = 上一次复位的来源；
     * 读完立刻清 RMVF。判读表与命令行替代取证见 docs/lessons.md#reset-forensics */
    Dbg_Printf("[boot] RCC_CSR=0x%08x  IWDG=%u WWDG=%u SFT=%u POR=%u PIN=%u LPWR=%u\r\n",
               (unsigned)RCC->CSR,
               (unsigned)((RCC->CSR >> 29) & 1U), (unsigned)((RCC->CSR >> 30) & 1U),
               (unsigned)((RCC->CSR >> 28) & 1U), (unsigned)((RCC->CSR >> 27) & 1U),
               (unsigned)((RCC->CSR >> 26) & 1U), (unsigned)((RCC->CSR >> 31) & 1U));
    RCC->CSR |= RCC_CSR_RMVF;      /* 清标志，让下一轮读到的是"那一次" */
    Dbg_Printf("[boot] reset flags cleared\r\n");
#endif

    LcdShow_Init();

    LightSensor_Init();                 /* GPIOF 时钟 + PF8 模拟输入 + ADC3 + 校准 */
    Control_Init();                     /* 执行器断开、蜂鸣器不响（BEEP(0)） */

    (void)EEPROM_Load(&th, &hys);       /* 三重校验失败回退默认值，不阻塞启动 */
    Control_Apply(th, hys, MODE_AUTO);
    Dbg_Printf("[cfg] th=%u hys=%u (EEPROM or default)\r\n", (unsigned)th, (unsigned)hys);

    Esp8266_Init(WIFI_SSID, WIFI_PASSWORD, TCP_SERVER_IP, TCP_SERVER_PORT);
    Dbg_Printf("[cfg] wifi ssid=%s target=%s:%u\r\n", WIFI_SSID, TCP_SERVER_IP,
               (unsigned)TCP_SERVER_PORT);

    /* 启动完成提示：短闪 2 下（约 1.1 s）。**必须放在开狗之前** —— IWDG 一旦启动就开始
     * 倒计时，这段闪烁会白吃掉超时余量。当前只保留这一次可见提示（进度码 3~7 已删除，
     * 见 docs/lessons.md#boot-progress）。 */
    Led0_BlinkMark(2);
    Dbg_Printf("[boot] boot-blink done; starting IWDG then entering main loop\r\n");

    /* 看门狗：放在所有开机耗时步骤之后、主循环正前方。太早开狗 + 超时太短 = 开机路径上
     * 每一步都被咬；又因 F1 的 IWDG 跨系统复位存活，会变成永久复位循环。
     * 失败不致命。详见 docs/lessons.md#iwdg-start-late */
    if (MX_IWDG_Init() != HAL_OK) {
        Dbg_Printf("[warn] IWDG init failed (non-fatal)\r\n");
    }

    last_tick = HAL_GetTick();

    for (;;) {
        Iwdg_Feed();

        if ((uint32_t)(HAL_GetTick() - last_tick) >= SYS_TICK_MS) {
            last_tick = HAL_GetTick();

            if (++hb_cnt >= HEARTBEAT_TOGGLE_SEC) {   /* 程序没卡死的最直观证据 */
                hb_cnt = 0;
                Led0_Toggle();
            }

            g_adc   = Light_ReadRaw(ADC_LIGHT_AVG_TIMES);   /* 8 次均值，≈0.17 ms */
            g_light = LIGHT_FROM_ADC(g_adc);                /* 4095 - adc */
            LcdShow_Update(g_light, g_th, g_relay, g_mode);
            Protocol_SendReport(NODE_ID, g_adc, g_light, g_th, g_hys,
                                g_relay, g_buzz, g_mode);   /* 同时充当链路心跳 */
            /* 本地闭环状态流：手电照射/遮挡时应看到 light 反向变化，可据此验证 T01 */
            Dbg_Printf("[1s] adc=%u light=%u th=%u hys=%u relay=%u buzz=%u mode=%u net=%u\r\n",
                       (unsigned)g_adc, (unsigned)g_light, (unsigned)g_th, (unsigned)g_hys,
                       (unsigned)g_relay, (unsigned)g_buzz, (unsigned)g_mode,
                       (unsigned)Esp8266_IsConnected());
        }

        Esp8266_Poll();                              /* 非阻塞：AT 状态机 + 重连退避 */
        while (Protocol_PopCommand(cmd_buf, sizeof(cmd_buf))) {
            Protocol_ParseAndExecute(cmd_buf);       /* 取出完整帧再执行 */
        }

        Control_Loop(g_light);                       /* 回差 + 手动优先，每轮都跑 */
    }
}

/**
 * @brief HAL 初始化回调（覆盖 stm32f1xx_hal.c:200-205 的 __weak 空实现）。
 *
 * 参考工程把这几个时钟放在 HAL_MspInit 里，我们必须实现才有 AFIO 时钟与 SWD-only 裁剪。
 * 本函数在 HAL_Init 设完分组与 SysTick 优先级**之后**才被调用，因此在这里改优先级分组
 * 属于隐蔽陷阱，**刻意不动分组**。详见 docs/lessons.md#nvic-grouping
 */
void HAL_MspInit(void)
{
    /* AFIO 时钟：使用外部中断线 / 引脚重映射 / 关闭 JTAG 都需要它。
     * HAL 默认不开，必须显式使能。 */
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    /* 释放 PA15 / PB3 / PB4（复位后默认被 JTAG 占用），只保留 SWD。
     * 本工程目前不使用这三个脚，但先裁掉可以避免将来出现
     * "代码里配了 GPIO 却怎么都不生效" 这类最难查的问题（PB4 = NJTRST）。
     * SWD（PA13/PA14）不受影响，Keil 下载与调试照常。 */
    __HAL_AFIO_REMAP_SWJ_NOJTAG();
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        /* 初始化失败：停在这里等看门狗复位，便于定位 */
    }
}
