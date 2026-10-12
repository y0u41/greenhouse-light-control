/**
 * @file main.c
 * @brief 主流程：初始化、1 s 周期采集/上报、事件调度、喂狗。
 *
 * 初始化顺序与主循环节拍严格按 Tech_Design §固件设计 落地：
 *   HAL_Init → 时钟(72 MHz, ADCCLK 12 MHz) → IWDG(≈2 s)
 *   → LED0 → LCD → 光敏 → 执行器+蜂鸣器(安全态) → EEPROM 装载 → ESP8266 → 自动模式
 *   主循环：喂狗 → 1 s 采样/上报 + 心跳 → ESP8266 轮询 → 取帧执行 → 控制闭环
 *
 * 说明：定时节拍用 HAL_GetTick() 在主循环里比较实现，而非额外 TIM 中断。
 *      理由：少配置一个外设、杜绝"标志位被漏清"类问题，且 1 s 周期 +
 *      非阻塞 AT 状态机下抖动远小于 ADC 变化速度；需严格中断驱动时可改为 TIM2。
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

/* IWDG：LSI ≈40 kHz ÷ 64 = 625 Hz，重装 **5000** → 溢出 ≈8.0 s
 *  · 超时由 2.0 s 放宽到 8.0 s，照抄参考工程 User/App/app_main.c:136-137（PR=4/RLR=5000）。
 *    2.0 s 太紧：实测只要开机路径上任何一步超过 2 s 就会被狗复位，表现为
 *    "板子毫无反应"（见 main() 里 IWDG 启动位置那段教训）。
 *  · DBGMCU_CR_DBG_IWDG_STOP 同样照抄参考工程 app_main.c:132：让**调试器暂停 CPU 时
 *    冻结看门狗**。不加这行，在 Keil 里一下断点 CPU 就停喂，8 s 后必被复位 ——
 *    现象是"一进断点就重启、完全没法单步调试"。
 * 返回状态供调用者判断，**失败绝不致命**：IWDG 一旦启动就无法关闭，反复烧录后
 * 寄存器可能仍处于写保护窗口，HAL_IWDG_Init 会超时返回错误。若在这里
 * Error_Handler()，就变成"死循环 + 看门狗其实没跑起来"的最坏状态 —— 板子毫无反应、
 * 连复位都不会有。上板实测正是如此。 */
#ifndef DBGMCU_CR_DBG_IWDG_STOP
#define DBGMCU_CR_DBG_IWDG_STOP     0x00000100U
#endif

static HAL_StatusTypeDef MX_IWDG_Init(void)
{
    /* ---- 必须用 HAL_IWDG_Init，不能改成裸寄存器直写 ----
     * 踩坑记录（实测定位）：我曾把它改成"照抄参考工程的裸写序列"
     *     KR=0x5555(解锁) -> PR=4 -> RLR=5000 -> KR=0xCCCC(启动) -> KR=0xAAAA(喂)
     * 结果板子进入**约 6 秒周期的永久复位循环**（串口 18 秒内出现 3 次开机横幅，
     * 每次都停在 "[cfg] wifi" 之后、"[boot] entering main loop" 之前）。
     *
     * 原因：RM0008 规定 IWDG_PR/IWDG_RLR 写入后会进入影子寄存器，必须等
     * IWDG_SR 的 PVU/RVU 位清零才真正生效；**PVU/RVU 未清时的写入被直接忽略**。
     * 裸写序列里 PR/RLR 写完立刻 KR=0xCCCC/0xAAAA，没有任何等待 —— 于是
     * 分频器（PR=4）生效了，而重装值仍是 POR 默认 RLR=0x0FFF=4095，
     * 实际超时 = 4095×64/40000 ≈ **6.55 s**（而不是标称的 8.0 s）。
     * 更关键的是：**STM32F1 的 IWDG 一旦启动，系统复位并不会清除它**，
     * 所以每一轮复位都沿用上一次没配好的超时 -> 永久复位循环。
     *
     * HAL_IWDG_Init 内部恰好做了这件事（stm32f1xx_hal_iwdg.c:177-197）：
     *     KR=0xCCCC -> KR=0x5555 -> 写 PR/RLR -> **轮询 IWDG_SR 直到为 0**
     *     -> KR=0xAAAA，并带 38.4 ms 超时。
     * 所以这里必须回到 HAL 版本；喂狗仍用裸写（见 Iwdg_Feed），
     * 这样既不丢 SR 轮询，又不会出现"空句柄解引用写地址 0"的 HardFault。 */
    DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;      /* 调试暂停时冻结 IWDG */

    s_hiwdg.Instance       = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_64; /* 64 分频 -> 625 Hz */
    /* ★★ 重装值必须 ≤ 4095：IWDG_RLR 只有 **12 位**（RL[11:0]，RM0008 §18.4.3）★★
     * 踩坑记录（由 RCC_CSR 取证 + 行标定位，实测确证）：
     *   曾写 Reload=5000（0x1388），硬件只保留低 12 位 -> 实际 RLR=0x388=904，
     *   实际超时 = 904×64/40000 = **1.45 s**（我们却以为是 8.0 s）。
     *   而开狗之后还要跑 Led0_BlinkMark(2) ≈1.1~1.2 s，几乎吃满 1.45 s ->
     *   间歇被咬；又因 **F1 的 IWDG 跨系统复位存活**，每轮从头再来 -> 永久复位循环。
     *   串口取证：每次复位 RCC_CSR 都读出 IWDGRSTF=1，且始终停在 "B: after
     *   MX_IWDG_Init" 之后、"C: after boot-blink" 之前。
     * 4000×64/40000 = **6.4 s**，对"开狗到主循环首次喂狗 ≈1.2 s"有 5× 余量。 */
    s_hiwdg.Init.Reload    = 4000U;             /* 12 位上限内，实际约 6.4 s */
    return HAL_IWDG_Init(&s_hiwdg);             /* 内部会轮询 IWDG_SR */
}

void Iwdg_Feed(void)
{
    /* 直接写键值喂狗。**绝不要改回 HAL_IWDG_Refresh(&s_hiwdg)**：
     * MX_IWDG_Init() 已改为纯寄存器写法（照抄参考工程），不会给 s_hiwdg.Instance
     * 赋值，它永远是 NULL；HAL_IWDG_Refresh 展开后是
     *     WRITE_REG(hiwdg->Instance->KR, 0xAAAA)
     * 即"往地址 0x00000000 写 0xAAAA" —— 实测就是硬故障现场：
     *   CFSR.IMPRECISERR=1、HFSR.FORCED=1，异常栈帧里 R2 = 0x0000AAAA。
     * 参考工程同样用裸写（Drivers/BSP/ESP8266 与 app_main.c 里的 IWDG 段）。 */
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
 * 为什么需要它：实测板载串口（USART1→CH340）打不出任何字节，而一旦某步初始化
 * 卡死，板子就是"毫无反应"，无法区分卡在哪。用闪灯次数当进度码，不需要任何
 * 额外工具，肉眼即可读出卡点。
 * 判读：数"连续快闪几下"。1=HAL_Init 2=IWDG 3=时钟 4=LCD
 *       5=采集/控制 6=EEPROM 7=通信；此后转为 5 s 慢心跳（主循环已启动）。 */
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

#if SAFE_BLINK_TEST
/* ===================== 纯寄存器闪烁自检（诊断用，零 HAL 依赖） =====================
 * 刻意**不用** HAL_Init / HAL_Delay / SysTick / 任何中断 / 时钟配置：
 *   只开 GPIOB 时钟 + 配 PB5 推挽输出，然后用忙等循环翻转。
 *
 * 为什么这样设计：上板现象是"红灯常亮"（低电平点亮 => PB5 被拉低后不再翻转），
 * 说明代码执行过 Led0_Init + 一次 Toggle，然后卡在第一次 HAL_Delay(200) ——
 * 高度指向 SysTick 没产生中断（HAL_Init 用 SystemCoreClock 算重装值，值不对
 * 则 tick 永不前进，HAL_Delay 永久阻塞，也让 HAL_RCC_OscConfig 的 HSE 等待死等）。
 *
 * 判据：
 *   快闪 -> 核心确实在跑、烧录与 LED 都正常，问题在 HAL/SysTick/时钟这条链上；
 *   仍不闪 -> PB5 之外的原因（供电/复位/启动映射），与 HAL 无关。
 */
int main(void)
{
    volatile uint32_t i;

    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;          /* 开 GPIOB 时钟（默认 HSI 8 MHz 即可） */
    GPIOB->CRL &= ~(0xFU << 20);                 /* PB5：清 CNF5/MODE5 */
    GPIOB->CRL |=  (0x3U << 20);                 /* PB5：推挽输出，50 MHz */
    GPIOB->BSRR = GPIO_BSRR_BS5;                 /* 先置高 = 灭（低电平点亮） */

    for (;;) {
        GPIOB->ODR ^= GPIO_ODR_ODR5;             /* 翻转 PB5 */
        for (i = 0; i < 60000U; i++) {           /* 粗忙等：HSI 8 MHz 下约 0.1~0.2 s */
            __NOP();
        }
    }
}
#else
int main(void)
{
    uint16_t th  = TH_DEFAULT;
    uint16_t hys = HYS_DEFAULT;
    uint32_t last_tick;
    uint8_t  hb_cnt = 0;
    static char cmd_buf[PROTOCOL_FRAME_MAX];

    /* ---- 启动顺序与参考工程（已在同一块板上跑通）完全一致 ----
     * 只保留最朴素的三步：HAL_Init -> SystemClock_Config -> 各外设初始化。
     *
     * 教训：之前我在此处额外加了三个"改进"——(a) 手工给 SystemCoreClock 赋值、
     * (b) 把 IWDG 提前到时钟配置之前、(c) 在配置时钟之前就初始化调试串口。
     * 结果是板子彻底毫无反应，而把这些去掉后与参考工程一致就能跑。
     * SystemCoreClock 的正确初值现在由已对齐的 system_stm32f1xx.c 提供（8000000），
     * 不需要在 main 里手工赋值。 */
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

    HAL_Init();
    SystemClock_Config();

    /* 中断优先级分组：**已回退，勿在此处恢复**。
     * 曾在此加 HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_2)（照抄参考工程
     * Core/Src/stm32f1xx_hal_msp.c:72），加上后板子进入**复位循环**：
     * 实测 IWDG 读回复位默认值 PR=0/RLR=0x0FFF（说明 MX_IWDG_Init 从未执行）、
     * g_th=0（Control_Apply 从未执行）、PC 在启动最开头与最末尾之间乱跳。
     * 回退后恢复正常，故确认是此行的位置问题。
     * 若将来要恢复，正确位置是 HAL_MspInit() 内部（由 HAL_Init() 调用、早于
     * HAL_InitTick），而**不是** HAL_Init() 之后 —— 放在后面会重新解释 SysTick
     * 已写入的优先级，而 delay_ms/lcd_init 的时序全挂在 SysTick 上。 */

    /* 必须在时钟配好之后调用：BSP 的 lcd_init() 内部用 delay_ms(50)/(5) 做上电与
     * 复位时序，若 g_fac_us 仍为 0 则这些延时全部失效 -> LCD 初始化不成功 -> 黑屏。
     * 本工程用的是 HAL 友好版 delay.c（只记录倍乘数、不接管 SysTick），
     * 因此 SysTick 归 HAL 独占，HAL_GetTick/HAL_Delay 与主循环节拍都不受影响。 */
    delay_init(72);

    /* LED0 只保留**一次短的"启动完成"提示**（末尾 2 闪）。
     * 原先进度码 3~7 共 25 次闪烁，使开机超过 25 秒，严重拖慢每轮验证与演示；
     * 那套码是当初为了定位"板子毫无反应"加的（半主机 HardFault），问题已解决，
     * 故按要求删除，只在末尾留一次可见提示。
     * 注：此前"C 串口在硬件层面不通"的结论是**误诊** —— 真实原因是半主机 bkpt
     * 触发 HardFault，固件根本没进过 main()。半主机已修复（见 retarget.c）。 */
    Led0_Init();
#if DEBUG_UART
    Dbg_Init();
    Dbg_Printf("[boot] clock ok, node=%u\r\n", (unsigned)NODE_ID);
    /* ================= 复位源取证 =================
     * RCC_CSR 的复位标志是**粘滞**的：只能由软件写 RMVF 清除、或由上电复位清除；
     * 普通的系统复位（含 IWDG）**不会**清掉它。因此：
     *   本次开机读到的标志 = **上一次复位**的来源。
     * 读完立刻清掉，这样下一次开机读到的就精确对应"那一次"复位。
     * bit31 LPWRRSTF / 30 WWDGRSTF / 29 IWDGRSTF / 28 SFTRSTF / 27 PORRSTF / 26 PINRSTF
     * 判读：
     *   IWDG=1                -> 看门狗咬的（核对 MX_IWDG_Init 的超时与喂狗间隔）
     *   PIN=1                 -> NRST 引脚被外部拉低（CH340 一键下载电路 / ST-Link / 复位键）
     *   POR=1                 -> 掉电复位（供电被瞬时大电流拉垮）
     *   SFTRST=1              -> 软件复位（NVIC_SystemReset）
     *   WWDG/LPWR             -> 窗口看门狗 / 低功耗（本工程都不该出现） */
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

    /* ---- 看门狗：必须放在**全部**开机耗时步骤之后才启动 ----
     * 实测教训（由 ST-Link 读 RCC_CSR 得到 IWDGRSTF=1 定位）：
     * IWDG 超时约 2.0 s，而 Led0_BlinkMark(6) 单项就约 2.4 s、
     * Esp8266_Init() 的 AT 探测更久。此前把 MX_IWDG_Init() 放在这些步骤**之前**，
     * 开机全程没有任何 Iwdg_Feed()，于是每约 2 s 被看门狗复位一次
     * -> 永远进不了主循环，外部表现就是"板子毫无反应"。
     * 主循环第一件事就是 Iwdg_Feed()，所以开机完成后再开狗是安全的。失败不致命。 */
    /* ================= BISECT STEP（临时，定位复位源） =================
     * 现象：板子反复复位（uwTick 采样 786 -> 133 -> 0 -> 0 -> 0），
     *       且 IWDG PR/RLR 读到的是硬件复位默认值 0/0x0FFF（= 从未被编程）。
     * 理论上不该是 IWDG，但为彻底排除，先整段禁用看门狗跑一轮。
     * 若禁用后恢复正常 -> 复位源就是 IWDG（需查为何 PR/RLR 读回默认值）；
     * 若仍然复位        -> 复位来自 NRST/上电等硬件侧，与固件无关。
     * 定位完成后必须恢复下面这段（发布固件必须有狗）。 */
    /* 启动完成提示：短闪 2 下（约 1.1 s）。**必须放在开狗之前** ——
     * IWDG 一旦启动就开始倒计时，这段闪烁会白吃掉超时余量。
     * 参考工程把 iwdg_start() 放在 while(1) 正前方、其后没有任何耗时动作，
     * 正是这个道理（它的 RLR=5000 被截成 904/1.45 s 也没暴露，纯属侥幸）。 */
    Led0_BlinkMark(2);
    Dbg_Printf("[boot] boot-blink done; starting IWDG then entering main loop\r\n");

    /* ---- 看门狗：放在所有开机耗时步骤之后、主循环正前方 ----
     * 实测教训（由 ST-Link 读 RCC_CSR 得到 IWDGRSTF=1 定位）：IWDG 超时若太短，
     * 开机路径上任何一步超时都会被咬；又因 F1 的 IWDG 跨系统复位存活，
     * 会变成永久复位循环（表现"板子毫无反应"）。失败不致命。 */
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

#endif /* SAFE_BLINK_TEST */

/**
 * @brief HAL 初始化回调（覆盖 stm32f1xx_hal.c:200-205 的 __weak 空实现）。
 *
 * 为什么要自己实现：参考工程把这几件事放在 HAL_MspInit 里，而我们此前完全没有，
 * 于是 AFIO 时钟没开、JTAG 也没裁成 SWD-only。
 *
 * ★ 关于优先级分组（重要，别在这里随手加）★
 * HAL_Init 的顺序是：stm32f1xx_hal.c:157 设分组(GROUP_4) -> :160 设 SysTick 优先级
 * -> :163 才调用本函数。也就是说**本函数在 SysTick 优先级已写入之后才执行**，
 * 在这里改分组只会让已经写好的 IPR 被"重新解释"，产生难以察觉的优先级错位
 * （参考工程就是这么做的，属于隐蔽陷阱）。我们保持 HAL 默认的 GROUP_4
 * （4 位全为抢占优先级），USART3 抢占 6 < SysTick 抢占 15，方向正确。
 * main.c 里曾试过在 HAL_Init 之后调用 SetPriorityGrouping(GROUP_2)，结果复位循环，
 * 已回退并留下"勿在此处恢复"的注释 —— 这里也**刻意不动分组**。
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
