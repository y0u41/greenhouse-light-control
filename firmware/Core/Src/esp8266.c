/**
 * @file esp8266.c
 * @brief ESP8266 AT 状态机：联网、TCP 透传、退避重连。
 *
 * 设计要点（Tech_Design §ESP8266 AT 状态机 / §接收行缓冲）：
 *  - 状态机：RST → CWMODE=1 → CWJAP → CIPSTART → CIPMODE=1(透传) → CONNECTED；
 *    失败/超时/收到 CLOSED → 退避重连（2 s → 4 s → 8 s … 上限 30 s）；
 *  - 全程非阻塞：Poll 里推进，等待过程仍可喂狗，绝不出现无喂狗的阻塞延时；
 *  - 透传模式下收到的数据不带 +IPD 前缀，透传期 RX 直接转交 Protocol_RxByte()；
 *  - 透传期仍监听整行 "CLOSED"（模块退出透传时会发该行），
 *    正常 JSON 负载不会出现裸 CLOSED 行，可安全区分；
 *  - 链路静默断开 STM32 侧感知不到，靠"1 s 上报发送失败/无响应"由上层发现，
 *    本模块保证重连，本地闭环不受影响。
 *
 * 硬件：**USART3  PB10(TX)/PB11(RX)**，115200 8N1 —— 即板卡 ATK 模块接口
 *       （模块 RX 接 PB10、模块 TX 接 PB11，交叉连接）。见下方 ESP_UART 宏块说明。
 */

#include "esp8266.h"
#include "protocol.h"
#include "debug_uart.h"

#include <stdio.h>

/* ===================== ESP8266 挂在哪一路 USART =====================
 * 硬件事实【依据板卡 IO 分配表】：正点原子精英版 V2 的 **ATK 模块接口**就是 USART3
 * —— PB10 = USART3_TX、PB11 = USART3_RX。本项目的 ESP8266 按该座子接线
 * （模块 RX <- PB10，模块 TX -> PB11），因此默认走 USART3。
 *
 * **不要想当然写成 USART2 / PA2-PA3**：那是另一组引脚（且与 RS485 经 P5 跳线复用），
 * 接错了的表现是"波特率、AT 指令、供电全都对，但一句 AT 都通不了"，极难排查。
 *
 * 若你的模块确实接在 PA2/PA3 上，只改下面这一组宏即可（其余代码不动）：
 *   #define ESP_UART              USART2
 *   #define ESP_UART_IRQn         USART2_IRQn
 *   #define ESP_UART_CLK_ENABLE() do { __HAL_RCC_USART2_CLK_ENABLE(); } while (0)
 *   #define ESP_GPIO_PORT         GPIOA
 *   #define ESP_GPIO_CLK_ENABLE() do { __HAL_RCC_GPIOA_CLK_ENABLE(); } while (0)
 *   #define ESP_TX_PIN            GPIO_PIN_2
 *   #define ESP_RX_PIN            GPIO_PIN_3
 *   #define ESP_IRQHandler        USART2_IRQHandler
 */
#define ESP_UART                USART3
#define ESP_UART_IRQn           USART3_IRQn
#define ESP_UART_CLK_ENABLE()   do { __HAL_RCC_USART3_CLK_ENABLE(); } while (0)
#define ESP_GPIO_PORT           GPIOB
#define ESP_GPIO_CLK_ENABLE()   do { __HAL_RCC_GPIOB_CLK_ENABLE(); } while (0)
#define ESP_TX_PIN              GPIO_PIN_10
#define ESP_RX_PIN              GPIO_PIN_11
#define ESP_IRQHandler          USART3_IRQHandler

static UART_HandleTypeDef s_esp_uart;
static uint8_t s_rx_byte;

typedef enum {
    ESP_RST = 0,
    ESP_CWMODE,
    ESP_CWJAP,
    ESP_CIPSTART,
    ESP_CIPMODE,
    ESP_CIPSEND,
    ESP_CONNECTED,
    ESP_BACKOFF
} esp_state_t;

static esp_state_t s_state = ESP_RST;
static uint32_t   s_t0;            /* 当前步骤起始时刻 */
static uint32_t   s_timeout_ms;    /* 当前步骤超时 */
static bool       s_sent;          /* 当前步骤是否已发指令 */
static bool       s_transparent;   /* 透传中：RX 转协议层 */
/* AT+CIPSEND 之后模块会输出一个 **不带换行** 的 '>' 提示符，见到它才算真正进入透传。
 * 这个字节在 ISR 里识别（行缓冲机制抓不到它），且**不能在 ISR 里做状态切换**
 * —— enter_state 会调 Dbg_Printf（阻塞式 HAL_UART_Transmit），在中断里做会与主循环
 * 抢串口。因此 ISR 只置标志，由 Esp8266_Poll 完成切换。 */
static volatile bool s_wait_prompt;  /* AT+CIPSEND 已发出，等待 '>' */
static volatile bool s_prompt_seen;  /* ISR 已见到 '>'，待 Poll 处理 */
/* 退避翻倍基数：进入 BACKOFF 时先翻倍得到本次退避值，
 * 故初值 1000 → 首次退避 2 s，随后 4/8/16 s，上限 30 s（与 Tech_Design 一致）。 */
static uint32_t   s_backoff_ms = 1000U;

static char s_ssid[33];
static char s_pwd[65];
static char s_ip[16];
static uint16_t s_port;

/* AT 响应行缓冲（同时用于透传期 CLOSED 检测） */
static char s_at_line[128];
static uint16_t s_at_len;
static volatile bool s_at_ready;
/* 透传期只嗅探很短的前缀：真正的控制行（CLOSED / WIFI DISCONNECT）都很短，
 * 而 JSON 负载远长于此。超出即判定为负载并丢弃到行尾 —— 既省去无谓拷贝，
 * 也杜绝负载里恰好含 "CLOSED" 子串造成的误重连。 */
#define AT_LINE_SNIFF_MAX   32U
static bool s_at_ignore;           /* 当前行已判定为负载：忽略到行尾 */

/* ===================== 底层收发 ===================== */

static void esp_uart_write(const uint8_t *data, uint16_t len)
{
    while (len > 0U) {
        uint16_t chunk = (len > 32U) ? 32U : len;

        if (HAL_UART_Transmit(&s_esp_uart, (uint8_t *)data, chunk, 100) != HAL_OK) {
            return;                          /* 发送失败：由超时/重连兜底 */
        }
        Iwdg_Feed();                         /* 长帧分块发，块间喂狗 */
        data += chunk;
        len  -= chunk;
    }
}

static void at_send(const char *cmd)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", cmd);

    if (n > 0 && n < (int)sizeof(buf)) {
        Dbg_Printf("[at] => %s\r\n", cmd);
        esp_uart_write((const uint8_t *)buf, (uint16_t)n);
    }
}

static void MX_EspUart_Init(void)
{
    GPIO_InitTypeDef gi = {0};

    ESP_UART_CLK_ENABLE();
    ESP_GPIO_CLK_ENABLE();

    gi.Pin   = ESP_TX_PIN;                    /* PB10 = USART3_TX -> 模块 RX */
    gi.Mode  = GPIO_MODE_AF_PP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ESP_GPIO_PORT, &gi);

    gi.Pin  = ESP_RX_PIN;                     /* PB11 = USART3_RX <- 模块 TX */
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLUP;                    /* 模块未接/断线时避免悬空噪声产生假起始位 */
    HAL_GPIO_Init(ESP_GPIO_PORT, &gi);

    s_esp_uart.Instance        = ESP_UART;
    s_esp_uart.Init.BaudRate   = 115200;      /* 正点原子 ATK-ESP8266 出厂默认 */
    s_esp_uart.Init.WordLength = UART_WORDLENGTH_8B;
    s_esp_uart.Init.StopBits   = UART_STOPBITS_1;
    s_esp_uart.Init.Parity     = UART_PARITY_NONE;
    s_esp_uart.Init.Mode       = UART_MODE_TX_RX;
    s_esp_uart.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    if (HAL_UART_Init(&s_esp_uart) != HAL_OK) {
        /* ★ 不再调用 Error_Handler()：它是 __disable_irq() + 死循环，
         * 而本函数在 IWDG 启动**之前**被调用（见 main.c 启动顺序），
         * 触发即永久挂死且不会复位。联网是"尽力而为"，失败必须降级：
         * 这里直接返回，后续 HAL_UART_Transmit 会因 gState != READY 返回 HAL_BUSY
         * （HAL 先查 gState 再碰 Instance，因此不存在空指针解引用），
         * 本地采集/显示/控制全部照常运行。 */
        Dbg_Printf("[esp] USART3 init FAILED (non-fatal; check PB10/PB11 & GPIOB clock)\r\n");
        return;
    }

    HAL_NVIC_SetPriority(ESP_UART_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(ESP_UART_IRQn);

    (void)HAL_UART_Receive_IT(&s_esp_uart, &s_rx_byte, 1);
}

/* CubeMX 生成的 stm32f1xx_it.c 里若也有同名的 USART3_IRQHandler，
 * 会与本文件重复定义导致链接报 multiple definition；删掉 it.c 里那个即可
 * （firmware/prebuild_check.py 会检查这一点）。 */
void ESP_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_esp_uart);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP_UART) {
        Esp8266_UartRxByte(s_rx_byte);
        (void)HAL_UART_Receive_IT(&s_esp_uart, &s_rx_byte, 1);
    }
}

/**
 * @brief 串口差错回调 —— **这是"能发不能收、突然再也连不上"的根治点**。
 *
 * 为什么必须实现它（照抄参考工程 Drivers/BSP/ESP8266/esp8266.c:150-160）：
 *   STM32F1 的 USART 一旦出现 ORE（溢出）/ FE / NE / PE，HAL 在
 *   HAL_UART_IRQHandler() 里会**终止本次接收**：把 RxState 置回 READY、
 *   调用本回调，并且**不会自动重挂 HAL_UART_Receive_IT()**。
 *   于是从此以后 RXNE 再也没人取、AT 行缓冲永久收不到任何字节 ——
 *   表现就是"AT 命令能发出去、模块的回应一个都收不到，且再也连不上"。
 *
 * 本工程确实会触发 ORE：Poll 里有"关中断取走整行"的临界区（见下），
 * 若期间连来两个字节就会溢出。因此这里必须清标志 + 强行重挂。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP_UART) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        huart->RxState   = HAL_UART_STATE_READY;      /* 让 Receive_IT 能重新武装 */
        (void)HAL_UART_Receive_IT(huart, &s_rx_byte, 1);
    }
}

void Esp8266_UartRxByte(uint8_t b)
{
    /* AT+CIPSEND 的 '>' 提示符：不带换行，行缓冲抓不到，必须按字节识别。
     * 它不属于透传数据，也不该进行缓冲，直接吞掉并置标志（状态切换交给 Poll）。 */
    if (s_wait_prompt && b == '>') {
        s_wait_prompt = false;
        s_prompt_seen = true;
        return;
    }

    if (s_transparent) {
        Protocol_RxByte(b);                   /* 透传数据：直接进协议层 */
    }

    if (b == '\n') {
        if (!s_at_ignore) {
            s_at_line[s_at_len] = '\0';
            s_at_ready = (s_at_len > 0U);
        }
        s_at_len    = 0U;
        s_at_ignore = false;
    } else if (b == '\r') {
        /* 行尾 \r：忽略 */
    } else if (s_at_ignore) {
        /* 已判定为负载：丢弃到行尾 */
    } else if (s_at_len >= (sizeof(s_at_line) - 1U)
               || (s_transparent && s_at_len >= AT_LINE_SNIFF_MAX)) {
        s_at_len    = 0U;
        s_at_ignore = true;
    } else {
        s_at_line[s_at_len++] = (char)b;
    }
}

/* ===================== 状态机 ===================== */

static const char *state_name(esp_state_t st)
{
    switch (st) {
    case ESP_RST:       return "RST";
    case ESP_CWMODE:    return "CWMODE";
    case ESP_CWJAP:     return "CWJAP";
    case ESP_CIPSTART:  return "CIPSTART";
    case ESP_CIPMODE:   return "CIPMODE";
    case ESP_CIPSEND:   return "CIPSEND";
    case ESP_CONNECTED: return "CONNECTED";
    case ESP_BACKOFF:   return "BACKOFF";
    default:            return "?";
    }
}

static void enter_state(esp_state_t st, uint32_t timeout_ms)
{
    Dbg_Printf("[at] state %s -> %s (timeout=%ums)\r\n",
               state_name(s_state), state_name(st), (unsigned)timeout_ms);
    s_state      = st;
    s_t0         = HAL_GetTick();
    s_timeout_ms = timeout_ms;
    s_sent       = false;
    s_transparent = (st == ESP_CONNECTED);
}

static void on_link_lost(void)
{
    s_transparent = false;
    s_wait_prompt = false;           /* 清掉残留标志，别让它吞掉下一次的 '>' */
    s_prompt_seen = false;
    /* ★ 同时清掉 protocol.c 的**行装配器与接收环**。
     * 断链若正好停在一帧 JSON 中途，protocol.c 的 s_line 会留下半帧且 s_line_len > 0；
     * 而它只在"成帧后"和"溢出时"清零 —— 重连后新字节会被**追加到旧半帧之后**，
     * 于是重连后的第一条指令必然解析失败并被静默丢弃（上位机只看到 ack 超时，
     * 完全不知道帧被吞了）。esp8266.c 自己的 AT 状态这里清得很完整，
     * 但协议层的残留此前无人负责 —— 这就是那条断层。 */
    Protocol_Reset();
    if (s_state != ESP_BACKOFF) {
        s_backoff_ms = (s_backoff_ms * 2U > 30000U) ? 30000U : (s_backoff_ms * 2U);
    }
    Dbg_Printf("[at] link lost -> backoff %ums\r\n", (unsigned)s_backoff_ms);
    enter_state(ESP_BACKOFF, 0);      /* 退避时长单独用 s_backoff_ms 计 */
}

static void send_step_cmd(void)
{
    char buf[128];

    switch (s_state) {
    case ESP_RST:
        /* ★ 先尝试"逃出透传模式"，再复位模块 ★
         *
         * 实测踩到的坑：上一次运行正常结束在 AT+CIPMODE=1 透传态，而**重新烧录 MCU
         * 并不会复位 ESP8266 模块**（模块由整板供电，只有整板断电才复位它）。
         * 模块停在透传态时，任何 AT 命令都不会被解释 —— 会被当成 TCP 数据原样发出去，
         * 于是 AT+RST 永远等不到 "ready"，表现为 RST 状态 8 s 超时 -> 无限退避、
         * net 恒为 0（串口只会不停刷 "state BACKOFF -> RST"）。
         *
         * 标准退出手法：发**裸 "+++"**（前后各需 ≥20 ms 静默，且**绝不能带 CR/LF**，
         * 否则不是转义序列）。所以这里不能用 at_send("+++") —— 它会补成 "+++\r\n"。
         *
         * 两种情况都要照顾：
         *  · 模块确实卡在透传态 -> "+++" 让它回到命令态并回 OK；
         *  · 模块本来就在命令态   -> "+++" 会被当成半条命令，必须再补一个裸 CRLF
         *                            把这条残行结束掉（模块回 ERROR，无害），
         *                            否则随后的 "AT+RST" 会被拼成 "+++AT+RST" 而失败。 */
        esp_uart_write((const uint8_t *)"+++", 3U);
        {
            uint32_t t0 = HAL_GetTick();
            while ((HAL_GetTick() - t0) < 25U) {   /* +++ 之后需 ≥20 ms 静默 */
                Iwdg_Feed();
            }
        }
        esp_uart_write((const uint8_t *)"\r\n", 2U);   /* 结束可能残留的 "+++" 半行 */
        {
            uint32_t t0 = HAL_GetTick();
            while ((HAL_GetTick() - t0) < 10U) {
                Iwdg_Feed();
            }
        }
        Dbg_Printf("[at] sent raw '+++' (escape transparent mode if module was stuck)\r\n");
        at_send("AT+RST");
        break;
    case ESP_CWMODE:
        at_send("AT+CWMODE=1");
        break;
    case ESP_CWJAP:
        (void)snprintf(buf, sizeof(buf), "AT+CWJAP=\"%s\",\"%s\"", s_ssid, s_pwd);
        at_send(buf);
        break;
    case ESP_CIPSTART:
        /* 建链前置准备（照抄参考工程 esp8266.c:482-496 的思路，但**顺序修正**）。
         *
         * ★★ 关键：AT+CIPMODE=1（透传模式）必须放在 AT+CIPSTART **之前** ★★
         * 实测（串口日志）：先 CIPSTART 再 CIPMODE=1 时，CIPSTART 能拿到 CONNECT、
         * CIPMODE=1 也回 OK，但紧接着的 AT+CIPSEND 会直接回 **ERROR** ——
         * 于是永远进不了透传，report 发不出去、PC 的下行也进不来。
         * 原因：CIPMODE 必须在建链时就确定，建链之后再切不会让模块重新进入
         * "透传待发"状态。正确顺序（Espressif AT 指令集）：
         *     AT+CIPMUX=0  ->  AT+CIPMODE=1  ->  AT+CIPSTART=...  ->  CONNECT
         *     ->  AT+CIPSEND  ->  OK  ->  '>'
         * 因此 ESP_CIPMODE 状态已不再使用（保留枚举只为兼容），
         * CIPSTART 成功后直接进 ESP_CIPSEND。 */
        at_send("AT+CIPMUX=0");
        at_send("AT+CIPMODE=1");
        (void)snprintf(buf, sizeof(buf), "AT+CIPSTART=\"TCP\",\"%s\",%u", s_ip, (unsigned)s_port);
        at_send(buf);
        break;
    case ESP_CIPMODE:
        at_send("AT+CIPMODE=1");
        break;
    case ESP_CIPSEND:
        /* 进入透传的第二步：必须在 CIPMODE=1 之后再发 CIPSEND，
         * 模块回 OK 并输出 '>' 提示符后才真正透传。
         *
         * 发之前**先清干净**（对齐参考工程 esp8266.c:504-520 的 esp8266_clear()）：
         * 若上一轮残留的 '>' 还挂在标志/缓冲里，本次会被误判为"透传已开"，
         * 于是模块实际仍在 AT 模式 —— 板子写出的 JSON 被当成 AT 命令吃掉，
         * 上行一条都到不了 PC；PC 下发的数据则变成 +IPD,<len>:<data> 文本，
         * 板子也解析不出命令（双向 0 帧）；堆积的 AT 文本最终还会让模块报错/复位，
         * PC 端看到的就是 WinError 10054。 */
        s_wait_prompt = false;
        s_prompt_seen = false;
        s_at_ready    = false;
        s_at_len      = 0U;
        s_at_ignore   = false;
        s_wait_prompt = true;
        at_send("AT+CIPSEND");
        break;
    default:
        break;
    }
}

/* ===================== AT 响应解析：整行精确匹配 =====================
 * 绝不用 strstr 子串匹配。AT 模块会把**异步状态行**（ready / WIFI CONNECTED /
 * WIFI GOT IP / WIFI DISCONNECT …）和**命令回显**混在响应流里，子串匹配很容易被
 * 这些行的内容误命中，把状态机推到错误分支 —— 这是 AT 解析最典型的坑。
 * line_is() 只接受"跳过首尾空白后与期望串完全相同"的行。 */
static bool line_is(const char *line, const char *expect)
{
    size_t n;

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    n = strlen(expect);
    if (strncmp(line, expect, n) != 0) {
        return false;
    }
    line += n;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return (*line == '\0');
}

static void handle_at_line(const char *line)
{
    /* 诊断关键：模块回的每一行都打出来。
     * 连不上时看这里就能分清是"模块没响应"（一行都没有）、
     * "WiFi 连不上"（CWJAP 回 FAIL）还是"TCP 连不上"（CIPSTART 回 ERROR）。 */
    Dbg_Printf("[at] <= %s\r\n", line);

    /* ---- 链路断开判定（对齐参考工程 atk_f103 的成熟做法）----
     * 触发条件只允许这几类，**绝不**把"我发出去的数据没被应答"当断线
     * （参考工程 app_mqtt.c 明确不做此判断——静默 ≠ 断线）：
     *  1) 模块 URC：CLOSED（TCP 被对方或被自己关掉）、WIFI DISCONNECT（掉 AP）；
     *  2) 模块**自身复位/重关联的指纹**：在已经建链（CIPSTART 之后）的状态下，
     *     突然又冒出 ready / WIFI CONNECTED / WIFI GOT IP —— 这三行只可能出现在
     *     模块刚启动或重新关联时。模块复位会把 TCP 单方面丢掉，而板子完全察觉不到，
     *     表现为：PC 端 WinError 10054（对端强制关闭），但板上 s_transparent 仍为 1，
     *     抱着一个死 socket 永远不重连。
     *     依据：参考工程 Tools/ESP8266_AT固件烧录说明.txt §六 指出模块发射瞬时电流
     *     200~300 mA 会拉崩 3.3V 造成欠压复位，是"link lost 反复出现"的第一嫌疑。
     * 注意：在 ESP_RST/CWMODE/CWJAP 阶段这些行是**正常**的，不能在这里拦截
     * （RST 阶段靠 ready 推进，CWJAP 阶段靠 WIFI GOT IP 推进）。 */
    if (line_is(line, "CLOSED") || line_is(line, "WIFI DISCONNECT")
        || ((s_state == ESP_CIPSTART || s_state == ESP_CIPMODE
             || s_state == ESP_CIPSEND  || s_state == ESP_CONNECTED)
            && (line_is(line, "ready") || line_is(line, "WIFI CONNECTED")
                || line_is(line, "WIFI GOT IP")))) {
        if (s_state == ESP_CONNECTED || s_state == ESP_CIPSTART
            || s_state == ESP_CIPMODE || s_state == ESP_CIPSEND) {
            Dbg_Printf("[at] URC while linked -> link lost (module reset?)\r\n");
            on_link_lost();
            return;
        }
    }

    switch (s_state) {
    case ESP_RST:
        /* ---------------- 关键修复（由串口实测日志定位）----------------
         * 绝不能因为 AT+RST 自己的回显 "OK" 就往下走：那个 OK 是模块**开始重启
         * 之前**就回出来的，此时模块还没起来，紧接着发的 AT+CWMODE=1 会被正在
         * 重启的模块直接丢弃。后果是 CWMODE 步 3 s 超时 -> 误判 link lost ->
         * 退避 30 s -> **永远走不到 CWJAP**，TCP 自然永远连不上。
         *
         * 实测日志（修复前的真实输出，症状照抄）：
         *   [at] => AT+RST
         *   [at] <= OK
         *   [at] state RST -> CWMODE (timeout=3000ms)
         *   [at] => AT+CWMODE=1
         *   [at] <= WIFI DISCONNECT / ready / WIFI CONNECTED / WIFI GOT IP
         *   [at] link lost -> backoff 30000ms          <-- 卡死在此循环
         *
         * 所以这里**只认重启完成横幅 "ready"**。
         * 兼容性：个别固件不打印 ready，故额外允许"AT+RST 之后 ≥1.5 s 才出现的
         * OK"（AT+RST 自己的 OK 在几十 ms 内就回，会被这个时间闸挡掉）。 */
        if (line_is(line, "ready")) {
            enter_state(ESP_CWMODE, 5000);
        } else if (line_is(line, "OK") && ((uint32_t)(HAL_GetTick() - s_t0) > 1500U)) {
            enter_state(ESP_CWMODE, 5000);
        }
        break;
    case ESP_CWMODE:
        if (line_is(line, "OK") || line_is(line, "no change")) {
            enter_state(ESP_CWJAP, 20000);
        } else if (line_is(line, "ERROR")) {
            on_link_lost();
        }
        break;
    case ESP_CWJAP:
        if (line_is(line, "OK") || line_is(line, "WIFI GOT IP")) {
            enter_state(ESP_CIPSTART, 15000);   /* 对齐参考工程 esp8266.c:494 的 15 s */
        } else if (line_is(line, "FAIL") || line_is(line, "ERROR")) {
            on_link_lost();
        }
        break;
    case ESP_CIPSTART:
        /* ---- 关键修复：AT+CIPSTART 是**异步**的，绝不能把 OK 当成功 ----
         * ESP8266 的 TCP 建链是两步：
         *     AT+CIPSTART="TCP","ip",port
         *     OK          <- 只是"命令已被接受"，此时 TCP 还在三次握手
         *     CONNECT     <- 这个才是真正建链成功
         * 失败时第二步回 ERROR / CLOSED / CONNECT FAIL。
         *
         * 修复前这里对 OK 也放行，后果是：TCP 根本没连上，状态机却一路冲进
         * CIPMODE -> CONNECTED 并把 s_transparent 置 1，之后抱着一个**并不存在**
         * 的连接永远不重连。
         * 实测症状（三者同时出现即可确诊本缺陷）：
         *   · 板上 s_state=5(CONNECTED)、s_transparent=1、s_backoff_ms=1000（从未失败）
         *   · PC 侧 Get-NetTCPConnection 看不到任何 Established 连接
         *   · 监听端（hil_probe）等不到节点接入
         * 所以这里**只认 CONNECT / ALREADY CONNECTED**。 */
        if (line_is(line, "CONNECT") || line_is(line, "ALREADY CONNECTED")) {
            /* 直接进 CIPSEND：透传模式已在 CIPSTART 之前设好（见 send_step_cmd），
             * 所以不再有单独的 CIPMODE 步骤。 */
            enter_state(ESP_CIPSEND, 5000);
        } else if (line_is(line, "ERROR") || line_is(line, "CONNECT FAIL")
                   || line_is(line, "CLOSED")) {
            on_link_lost();
        }
        break;
    case ESP_CIPMODE:
        if (line_is(line, "OK")) {
            /* ---- 关键修复：透传是两步，不能停在 CIPMODE ----
             * AT+CIPMODE=1 只是"允许透传"，必须再发 AT+CIPSEND 并等到 '>' 提示符，
             * 模块才真正进入透传。少了这一步的实测症状：TCP 已 Established、
             * s_transparent=1，但**双向都是 0 帧** —— 板子写出的 JSON 被模块当成
             * AT 命令丢弃，PC 下发的 cmd 也不会转发给板子（因此一个 ack 都没有）。 */
            enter_state(ESP_CIPSEND, 5000);     /* 对齐参考工程 esp8266.c:504-519 的 5 s */
        } else if (line_is(line, "ERROR")) {
            on_link_lost();
        }
        break;
    case ESP_CIPSEND:
        /* 正常路径：OK -> '>'（提示符在 ISR 里识别）。
         * 这里只处理失败分支；成功由 s_prompt_seen 在 Poll 里收尾。 */
        if (line_is(line, "ERROR") || line_is(line, "link is not valid")) {
            on_link_lost();
        }
        break;
    default:
        break;
    }
}

void Esp8266_Poll(void)
{
    /* AT+CIPSEND 的 '>' 已由 ISR 捕获：在这里（主循环上下文）安全地切状态。
     * ★ 这行日志是**关键取证**：串口里出现它才说明模块真的打印了 '>'、透传真的开了。
     *   若一直看不到它而状态却变成了 CONNECTED，就说明 '>' 检测被误触发。 */
    if (s_prompt_seen) {
        s_prompt_seen = false;
        Dbg_Printf("[at] got '>' prompt -> transparent ON\r\n");
        s_backoff_ms  = 1000U;               /* 连上后重置退避基数（下次失败仍是 2 s） */
        enter_state(ESP_CONNECTED, 0);
        return;
    }

    /* 每轮只处理一行响应，保持主循环节拍均匀。
     * 先整行拷出再清标志，缩短与 ISR 写入 s_at_line 的竞争窗口。 */
    if (s_at_ready) {
        char line[sizeof(s_at_line)];

        /* 与 RX 中断互斥地取走整行：否则 ISR 可能在 memcpy 中途覆盖 s_at_line，
         * 把"半行旧内容 + 半行新内容"拼成一行交给解析器，产生莫名其妙的解析结果。
         * 临界区只有一次 128 字节拷贝（µs 级），远小于 115200 下 1 字节的 87 µs，
         * 且只禁 USART3 这一个中断源，不会丢字节（RXNE 会保持到重新使能后进中断）。 */
        HAL_NVIC_DisableIRQ(ESP_UART_IRQn);
        memcpy(line, s_at_line, sizeof(line));
        s_at_ready = false;
        /* ★ 重开中断之前必须清 ORE 并重挂接收（照抄参考工程 esp8266.c:167-171）。
         * 上面的临界区若恰好跨过两个字节就会溢出；F1 上 ORE 会让 HAL 终止接收且
         * 不自动重挂，此后 AT 行缓冲永久停摆（"能发不能收"）。 */
        __HAL_UART_CLEAR_OREFLAG(&s_esp_uart);
        if (s_esp_uart.RxState == HAL_UART_STATE_READY) {
            (void)HAL_UART_Receive_IT(&s_esp_uart, &s_rx_byte, 1);
        }
        HAL_NVIC_EnableIRQ(ESP_UART_IRQn);
        handle_at_line(line);
        return;
    }

    if (!s_sent && s_state != ESP_BACKOFF && s_state != ESP_CONNECTED) {
        s_sent = true;
        send_step_cmd();
    }

    if (s_state == ESP_BACKOFF) {
        if ((uint32_t)(HAL_GetTick() - s_t0) >= s_backoff_ms) {
            enter_state(ESP_RST, 8000);
        }
        return;
    }

    if (s_state == ESP_CONNECTED) {
        return;                              /* 透传期无 AT 交互 */
    }

    if (s_timeout_ms > 0U && (uint32_t)(HAL_GetTick() - s_t0) > s_timeout_ms) {
        on_link_lost();                      /* 每步都有超时，绝不无限等待 */
    }
}

bool Esp8266_IsConnected(void)
{
    return (s_state == ESP_CONNECTED) && s_transparent;
}

void Esp8266_SendRaw(const char *data, uint16_t len)
{
    if (!s_transparent) {
        return;                              /* 未透传：丢弃，本地闭环不受影响 */
    }
    esp_uart_write((const uint8_t *)data, len);
}

void Esp8266_Init(const char *ssid, const char *pwd, const char *ip, uint16_t port)
{
    strncpy(s_ssid, (ssid != NULL) ? ssid : "", sizeof(s_ssid) - 1U);
    strncpy(s_pwd,  (pwd  != NULL) ? pwd  : "", sizeof(s_pwd)  - 1U);
    strncpy(s_ip,   (ip   != NULL) ? ip   : "", sizeof(s_ip)   - 1U);
    s_port = (port != 0U) ? port : 9092U;

    MX_EspUart_Init();
    enter_state(ESP_RST, 8000);              /* 失败不阻塞，本地闭环优先 */
}
