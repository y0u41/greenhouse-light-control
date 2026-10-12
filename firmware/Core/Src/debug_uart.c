/**
 * @file debug_uart.c
 * @brief 上板诊断串口：USART1 (PA9/PA10) -> 板载 CH340C -> PC COM 口，115200 8N1。
 *
 * 设计约束：
 *  - 只在**主循环上下文**里调用（esp8266 的 Poll / main 的 1 s 分支），
 *    绝不在中断里调用，避免阻塞收发中断。
 *  - DEBUG_UART=0 时编译为空实现，发布固件不引入任何串口开销。
 *  - 发送用阻塞式 HAL_UART_Transmit，但每段都很短（几十字节），
 *    且在 115200 下 20 字节约 1.7 ms，对 1 s 周期无影响。
 */

#include "debug_uart.h"
#include "main.h"

#if DEBUG_UART

#include <stdarg.h>
#include <string.h>

static UART_HandleTypeDef s_dbg;

void Dbg_Init(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gi.Pin   = GPIO_PIN_9;                    /* PA9  = USART1_TX -> CH340C RX */
    gi.Mode  = GPIO_MODE_AF_PP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gi);

    gi.Pin  = GPIO_PIN_10;                    /* PA10 = USART1_RX <- CH340C TX */
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gi);

    s_dbg.Instance        = USART1;
    s_dbg.Init.BaudRate   = 115200;
    s_dbg.Init.WordLength = UART_WORDLENGTH_8B;
    s_dbg.Init.StopBits   = UART_STOPBITS_1;
    s_dbg.Init.Parity     = UART_PARITY_NONE;
    s_dbg.Init.Mode       = UART_MODE_TX_RX;
    s_dbg.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    if (HAL_UART_Init(&s_dbg) != HAL_OK) {
        return;                                /* 调试口失败不影响业务 */
    }
}

void Dbg_Write(const char *s)
{
    if (s == NULL || *s == '\0') {
        return;
    }
    (void)HAL_UART_Transmit(&s_dbg, (uint8_t *)s, (uint16_t)strlen(s), 200);
}

void Dbg_Printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        buf[sizeof(buf) - 1] = '\0';
        Dbg_Write(buf);
    }
}

#else  /* DEBUG_UART == 0：空实现 */

void Dbg_Init(void) { }
void Dbg_Write(const char *s) { (void)s; }
void Dbg_Printf(const char *fmt, ...) { (void)fmt; }

#endif /* DEBUG_UART */
