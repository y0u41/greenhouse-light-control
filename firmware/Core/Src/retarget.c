/**
 * @file retarget.c
 * @brief 关闭 ARM C 库的半主机(semihosting)模式，并把 printf 底层出口重定向到调试串口。
 *
 * ============================ 为什么必须有这个文件 ============================
 * 本工程链接的是 ARM **完整标准 C 库**（工程 Define 中没有 __MICROLIB）。
 * 标准库在启动阶段 __rt_lib_init -> _initio 里会调用 _sys_open 去打开标准流，
 * 而库自带的 _sys_open 走 **半主机(semihosting)** 通道，底层指令就是 `bkpt 0x00AB`。
 *
 * 在**没有连接调试器**的独立运行场景下，Cortex-M3 执行 BKPT 会直接触发 **HardFault**，
 * 而 HardFault_Handler 是 `B .` 死循环。于是启动流程变成：
 *
 *     上电 -> Reset_Handler -> __scatterload -> _initio -> _sys_open -> bkpt 0xAB
 *          -> HardFault -> 死在 B .        （永远到不了 main()）
 *
 * 表面现象就是"板子毫无反应、LCD 不亮、串口一个字节都没有"，极易被误判成
 * 供电/时钟/接线/屏幕问题。**实测证据（ST-Link 读内核寄存器 + 异常栈帧）**：
 *
 *     PC = 0x0800551C (HardFault_Handler)      XPSR.IPSR = 3
 *     异常栈帧 @ 0x20001000 : PC = 0x0800164C , LR = 0x08001647
 *     0x0800164C 反汇编 = `bkpt 0x00ab`（正位于 _sys_open 内部）
 *
 * 本文件通过声明 __use_no_semihosting 并自行实现全部底层 _sys_* 接口，
 * 把这条通道彻底关掉。做法移植自参考工程
 * Drivers/SYSTEM/usart/usart_retarget.c（该工程已在其上验证可用）。
 *
 * ============================== 安全设计约束 ==============================
 * 参考工程的 fputc 是**无保护忙等** USART1 的 TC 位；一旦调试串口尚未初始化
 * （时钟未开、UE=0），该忙等会永久死等。本实现因此加了两道保护：
 *   1) USART1 未使能(UE=0) 时直接丢弃字符并返回，绝不触碰寄存器等待；
 *   2) 等待 TXE 带计数上限，任何情况下都不会死等。
 * 这样即便将来有人在 Dbg_Init() 之前调用 printf，也不会把系统卡死。
 */

#include <stdio.h>
#include "main.h"

/* ------------------------------------------------------------------ *
 * AC6 (armclang) / AC5 (armcc) 兼容地声明"不使用半主机模式"
 * ------------------------------------------------------------------ */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)
__asm(".global __use_no_semihosting\n\t");   /* 不使用半主机 */
__asm(".global __ARM_use_no_argv \n\t");     /* main 为无参形式 */
#else
#pragma import(__use_no_semihosting)

struct __FILE
{
    int handle;
};

#endif

/* 标准流对象：库需要它来承载 stdout */
FILE __stdout;

/* ------------------------------------------------------------------ *
 * 低层 _sys_* 接口：全部实现为空操作。
 * 由于这些符号由本文件提供，链接器不会再去拉取库里的半主机版本，
 * 因此不会再出现 `bkpt 0xAB`。
 * ------------------------------------------------------------------ */

int _sys_open(const char *name, int openmode)
{
    (void)name;
    (void)openmode;
    return -1;                      /* 不打开任何文件/设备 */
}

int _sys_close(int fh)
{
    (void)fh;
    return 0;
}

int _sys_write(int fh, const unsigned char *buf, unsigned len, int mode)
{
    (void)fh;
    (void)buf;
    (void)len;
    (void)mode;
    return 0;
}

int _sys_read(int fh, unsigned char *buf, unsigned len, int mode)
{
    (void)fh;
    (void)buf;
    (void)len;
    (void)mode;
    return 0;
}

int _sys_istty(int fh)
{
    (void)fh;
    return 0;
}

int _sys_seek(int fh, long pos)
{
    (void)fh;
    (void)pos;
    return -1;
}

long _sys_flen(int fh)
{
    (void)fh;
    return 0;
}

void _sys_exit(int x)
{
    (void)x;
    for (;;)                        /* 正常流程不会走到这里 */
    {
    }
}

int _ttywrch(int ch)
{
    return ch;
}

char *_sys_command_string(char *cmd, int len)
{
    (void)cmd;
    (void)len;
    return NULL;
}

/* ------------------------------------------------------------------ *
 * printf 底层字符出口 -> USART1（由 debug_uart.c 初始化为 115200 8N1）
 * ------------------------------------------------------------------ */
int fputc(int ch, FILE *f)
{
    uint32_t guard = 0x10000U;
    (void)f;

    if ((USART1->CR1 & USART_CR1_UE) == 0U)
    {
        return ch;                  /* 调试串口未就绪：丢弃，绝不阻塞 */
    }

    while (((USART1->SR & USART_SR_TXE) == 0U) && (guard-- != 0U))
    {
        ;
    }

    USART1->DR = (uint8_t)ch;
    return ch;
}
