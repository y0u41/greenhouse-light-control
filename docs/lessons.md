# 固件踩坑全记录（lessons）

> **本文件与 [`AGENTS.md`](../AGENTS.md) 的「已知陷阱」是什么关系？**
>
> - [`AGENTS.md`](../AGENTS.md)「已知陷阱」是**速查条目**：一句话讲清"结论 + 禁令"，
>   给"改代码前必读"用，篇幅有限、不写过程。
> - **本文件是完整来龙去脉**：现象 → 观测证据 → 根因 → 修法 → 复现方法。
>   给"想弄明白为什么"和"下次同类问题怎么定位"用。
> - 两者**刻意不重复**：AGENTS.md 里已编号的 27 条只在这里给出行号引用，
>   不再复述其内容。本文件收录的是那些"长到塞不进速查表、但删了就再也查不到"的经历。
>
> 涉及固件时，两个文件配套读：先看 AGENTS.md 得结论，需要证据与复现方法时翻到这里。

各节锚点（`main.c` 的注释按这些锚点指向本文件）：

| 锚点 | 主题 |
| --- | --- |
| [`#semihosting`](#semihosting) | 半主机 `bkpt` 让固件死在 `main()` 之前 |
| [`#iwdg`](#iwdg) | IWDG 一族坑（12 位重装值 / 跨复位存活 / SR 轮询 / 空句柄 / 冻结 / 启动位置） |
| [`#reset-forensics`](#reset-forensics) | 复位源取证：`RCC_CSR` 粘滞标志的正确读法 |
| [`#debug-only-once`](#debug-only-once) | 只信一次观测：ST-Link `hotplug` 会复位目标 |
| [`#nvic-grouping`](#nvic-grouping) | 中断优先级分组必须在 `HAL_Init` 之前落地 |
| [`#boot-order`](#boot-order) | 启动顺序：保持最朴素的三步 |
| [`#boot-progress`](#boot-progress) | 启动进度闪灯码（已删除，附恢复方法） |
| [`#lcd`](#lcd) | LCD 面板识别与"黑屏 ↔ 白屏"闪 |

---

## semihosting

**现象**：烧录 `Verify OK`，板子上电后**毫无反应** —— LCD 不亮、串口 **0 字节**、
连启动闪灯都不出。极易被误判成供电 / 时钟 / 接线 / 屏幕问题。

**根因**：工程链接的是 ARM 完整标准 C 库（`Define` 里没有 `__MICROLIB`）。标准库在
`__rt_lib_init → _initio → _sys_open` 阶段打开标准流，而库自带的 `_sys_open` 走
**半主机（semihosting）** 通道，底层指令就是 `bkpt 0x00AB`。**没有调试器时
Cortex-M3 执行 `BKPT` 直接触发 HardFault**，而 `HardFault_Handler` 是 `B .` 死循环
→ 永远到不了 `main()`。

**证据**（当时用 ST-Link 抓到的现场）：

- `PC = 0x0800551C (HardFault_Handler)`、`XPSR.IPSR = 3`（处于异常处理态）
- 异常栈帧 `@0x20001000`：`PC = 0x0800164C`、`LR = 0x08001647`
- `0x0800164C` 反汇编出来正是 `bkpt 0x00ab`（在 `_sys_open` 内部）

**修法**：`firmware/Core/Src/retarget.c` 不可删（`__use_no_semihosting` +
自行实现全部 `_sys_*`）。参考工程对应文件是 `Drivers/SYSTEM/usart/usart_retarget.c`。

**复现 / 自检**：

```bash
arm-none-eabi-objdump -d firmware/Output/greenhouse.axf | findstr bkpt   # 必须无输出
```

**⚠ 最大的观测陷阱**：**接上调试器时 `bkpt` 会被调试器接管**，现象与裸跑完全不同
（HardFault 断点反而不命中、`continue` 一直不返回）。所以**绝对不能用"接调试器能跑"
去否定这个原因** —— 当时正是因为这个误判，把问题错误地归因成了"板载串口在硬件层面
不通"，白绕了很久。**这个结论是误诊，不要再重复。**

（AGENTS.md「已知陷阱」#12 是速查版。）

## iwdg

IWDG 是本项目**最容易做出"板子毫无反应"**的外设，下面 6 条是实测踩出来的，
顺序按"配置一只正确的狗"的操作次序排。

### iwdg-reload-12bit —— 重装值必须 ≤ 4095

`IWDG_RLR` 只有 **12 位**（`RL[11:0]`，RM0008 §18.4.3）。

**踩坑记录**：曾写 `Reload = 5000`（`0x1388`），硬件只保留低 12 位
→ 实际 `RLR = 0x388 = 904` → 实际超时 `904 × 64 / 40000 = **1.45 s**`，
而我们一直以为是 8.0 s。

**为什么这会导致永久复位循环**：开狗之后还要跑 `Led0_BlinkMark(2) ≈ 1.1~1.2 s`，
几乎吃满 1.45 s → 间歇被狗咬；又因 **F1 的 IWDG 跨系统复位存活**（见下节），
每一轮复位都从头再来 → 永久循环。

**证据**（`RCC_CSR` 取证 + 行标定位）：

- 每次复位读 `RCC_CSR` 都是 `IWDGRSTF = 1`
- 复位点始终停在 `"B: after MX_IWDG_Init"` 之后、`"C: after boot-blink"` 之前

**修法**：`Reload = 4000U` → `4000 × 64 / 40000 = **6.4 s**`，对
"开狗到主循环首次喂狗 ≈ 1.2 s"有 5× 余量。**永远不要超过 4095。**

（AGENTS.md「已知陷阱」#2 是速查版。）

### iwdg-survives-reset —— 启动后系统复位不清它

**STM32F1 的 IWDG 一旦启动，系统复位（含按键复位、`NVIC_SystemReset`）不会清除它**
（只有上电复位 POR 会）。所以配置错了就是**永久复位循环**，每一轮沿用同一份错配置 ——
这解释了为什么"反复烧录 / 反复按复位键完全没用"。

**操作含义**：**改狗参数（PR / RLR）后必须整板断电重新上电再验证**，光按复位键不算验证。

（AGENTS.md「已知陷阱」#3 是速查版。）

### iwdg-sr-poll —— 写完 PR/RLR 必须轮询 IWDG_SR

`IWDG_PR` / `IWDG_RLR` 写入后先进入**影子寄存器**，必须等 `IWDG_SR` 的
`PVU` / `RVU` 位清零才真正生效；**`PVU` / `RVU` 未清时的写入被直接忽略**。

**踩坑记录**：曾把 `MX_IWDG_Init()` 改成"照抄参考工程的裸寄存器序列"：

```c
KR = 0x5555; PR = 4; RLR = 5000; KR = 0xCCCC; KR = 0xAAAA;
```

结果板子进入**约 6 秒周期的永久复位循环**（串口 18 秒内出现 3 次开机横幅，
每次都停在 `"[cfg] wifi"` 之后、`"[boot] entering main loop"` 之前）。

原因：裸写序列在 `PR` / `RLR` 写完后**立刻** `KR = 0xCCCC/0xAAAA`，中间没有任何等待
→ 分频器（`PR = 4`）生效了，而重装值仍是 POR 默认的 `RLR = 0x0FFF = 4095`
→ 实际超时 `4095 × 64 / 40000 ≈ **6.55 s**`（而不是标称的 8.0 s）。再叠加
"F1 的 IWDG 跨复位存活"，就成了永久循环。

`HAL_IWDG_Init()` 内部恰好做了这件事（`stm32f1xx_hal_iwdg.c:177-197`）：

```
KR=0xCCCC -> KR=0x5555 -> 写 PR/RLR -> 轮询 IWDG_SR 直到为 0 -> KR=0xAAAA
```

并带 38.4 ms 超时。**所以初始化必须用 HAL 版，不要为"照抄参考工程"改成裸写。**

> **参考工程里这是"成功案例中的隐藏 bug"**：它的 `RLR = 5000` 同样被截成 904
> （1.45 s），只是因为它把 `iwdg_start()` 紧贴 `while(1)`、其后没有任何耗时动作，
> 侥幸没暴露。**不要照抄成功案例里的可疑写法。**

（AGENTS.md「已知陷阱」#4 是速查版。）

### iwdg-refresh-null —— 喂狗用裸写，不要用 HAL_IWDG_Refresh

**踩坑记录**：`HAL_IWDG_Refresh(&s_hiwdg)` 在 `s_hiwdg.Instance == NULL` 时会写地址 0。
展开后就是：

```c
WRITE_REG(hiwdg->Instance->KR, 0xAAAA)   /* Instance == NULL -> 往 0x00000000 写 */
```

**实测现场**：`CFSR.IMPRECISERR = 1`、`HFSR.FORCED = 1`，异常栈帧里
`R2 = 0x0000AAAA`（正是要写的键值）。

**修法**：喂狗用裸写 `IWDG->KR = 0xAAAAU;`（不依赖句柄）。参考工程同样用裸写。

（AGENTS.md「已知陷阱」#5 是速查版。）

### iwdg-dbgmcu-freeze —— 调试时冻结看门狗

`DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP`（照抄参考工程 `User/App/app_main.c:132`）
让**调试器暂停 CPU 时冻结看门狗**。

**不加会发生什么**：在 Keil 里一下断点 CPU 就停止喂狗，**≈6.4 s** 后必被复位 ——
现象是"一进断点就重启、完全没法单步调试"。

**注意**：该宏在部分 CMSIS 头版本里没有定义，故 `main.c` 里自带
`#ifndef DBGMCU_CR_DBG_IWDG_STOP / #define 0x00000100U` 兜底。

### iwdg-start-late —— 启动放在所有耗时步骤之后

**现象**：固件反复重启，永远进不了主循环（表现为"板子毫无反应"或反复闪启动码）。

**根因**：`Iwdg_Feed()` 直到主循环第一行才被调用，而 `MX_IWDG_Init()` 曾放在
开机耗时步骤**之前**；超时又只有约 2.0 s，于是每约 2 s 被咬一次。

**证据**：`RCC_CSR = 0x3C000002`，其中 `IWDGRSTF`(bit29) = 1；
当时 `IWDG_RLR = 0x4E2 (1250)`、`IWDG_PR = 4 (÷64)` → `1250 × 64 / 40000 ≈ 2.0 s`。

**修法 / 规则**：

- `MX_IWDG_Init()` **只能紧邻 `for(;;)` 之前**；开机路径上任何 > 1.5 s 的步骤
  都要重新审视顺序。
- 启动提示闪灯（`Led0_BlinkMark(2)`，约 1.1 s）**必须放在开狗之前**，否则白吃超时余量。
- 初始化**失败不得致命**：IWDG 一旦启动就无法关闭，反复烧录后寄存器可能仍在写保护
  窗口，`HAL_IWDG_Init()` 会超时返回错误。此时若调 `Error_Handler()`
  （`__disable_irq() + for(;;)`），就变成"死循环 + 狗其实没跑起来"的最坏状态 ——
  板子毫无反应、**连复位都不会有**。上板实测正是如此，故只打印告警。

（AGENTS.md「已知陷阱」#13 是速查版；#17 讲的是同一条"驱动失败不得致命化"约束。）

## reset-forensics

**复位源取证：开机读 `RCC->CSR`。**

`RCC_CSR` 的复位标志是**粘滞**的：只能由软件写 `RMVF` 清除、或由上电复位清除；
**普通的系统复位（含 IWDG 复位）不会清掉它**。因此：

> **本次开机读到的标志 = 上一次复位的来源。**

读完**立刻** `RCC->CSR |= RCC_CSR_RMVF` 清掉，这样下一次开机读到的就精确对应"那一次"。

位定义与判读：

| 位 | 名称 | 判读 |
| --- | --- | --- |
| bit31 | `LPWRRSTF` | 低功耗复位（本工程不该出现） |
| bit30 | `WWDGRSTF` | 窗口看门狗（本工程不该出现） |
| bit29 | `IWDGRSTF` | 独立看门狗咬的 → 核对超时与喂狗间隔 |
| bit28 | `SFTRSTF` | 软件复位（`NVIC_SystemReset`） |
| bit27 | `PORRSTF` | 掉电复位（供电被瞬时大电流拉垮） |
| bit26 | `PINRSTF` | `NRST` 被外部拉低（CH340 一键下载电路 / ST-Link / 复位键） |

固件侧对应输出（`DEBUG_UART=1` 时）：

```
[boot] RCC_CSR=0x14000000  IWDG=0 WWDG=0 SFT=1 POR=0 PIN=1 LPWR=0
[boot] reset flags cleared
```

上例含义：上一次复位是 **ST-Link 烧录触发的软件复位（SFT）+ NRST 被拉低（PIN）**，
**没有看门狗复位（IWDG=0）** —— 这是"烧录后一次干净启动"应有的样子。

**这次就是靠它把"屏幕黑 ↔ 白闪"定位成 IWDG 复位循环的。**

**命令行替代取证**（不想烧诊断固件时）：

```powershell
STM32_Programmer_CLI.exe -c port=SWD mode=hotplug -r32 0x40021024 0x04   # RCC_CSR
```

⚠ `-r32` 的 `<size>` 参数**不可省略**；且 `mode=hotplug` 有复位目标的风险，见下节。

（AGENTS.md「已知陷阱」#16 是速查版。）

## debug-only-once

**先怀疑自己的测量工具，再怀疑被测系统。**

**SWD 的 `mode=hotplug` 在这台 ST-Link 上每次连接都可能复位目标。**
`STM32_Programmer_CLI` 启动本身需要 1~2 s，于是**读到的常常是"复位后刚开始启动"的瞬间**：

- `.data` 已加载（如 `g_back_color = 0xFFFF` 正确）
- 但 `.bss` 全 0、`uwTick = 0`

本次开发中曾据此得出**"LCD 面板未识别、宽高为 0"等完全错误的结论**。

**对策**：

1. **一次连接读回整块 RAM**（`-r32 0x20000000 0x2000`），让所有字段来自同一瞬间、
   彼此自洽；
2. 上板排查**优先用串口这个非侵入通道**；
3. 配套陷阱：2 字节对齐的结构体**不能**用 `-r32` 直接读它的地址 —— `lcddev` 位于
   `0x20000126`（2 字节对齐），32 位读会返回错值，必须**按 4 字节对齐读、再按字节位置拆字段**；
4. PowerShell 5.1 内置别名会吞命令：`r → Invoke-History`、`rd/Rd → Remove-Item`、
   `h/H → Get-History`。凡"读寄存器"的函数起名叫 `r` / `rd` / `h` 的脚本都会读到垃圾数据
   → **读数脚本一律用 Python 写**（`tools/probe.py`）。

（AGENTS.md「已知陷阱」#10、#11、#12 是速查版。）

## nvic-grouping

**中断优先级分组必须在 `HAL_Init()` 内部（`HAL_MspInit()`）落地，不要在 `HAL_Init()` 之后改。**

`HAL_Init` 的顺序是（`stm32f1xx_hal.c:157/160/163`）：

```
设分组(GROUP_4) -> 设 SysTick 优先级 -> 才调用 HAL_MspInit()
```

也就是说 **`HAL_MspInit()` 在 SysTick 优先级已写入之后才执行**，在这里改分组只会让
已经写好的 `IPR` 被"重新解释"，产生难以察觉的优先级错位（参考工程就是这么做的，
属于隐蔽陷阱）。

**踩坑记录**：曾在 `HAL_Init()` 之后加
`HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_2)`（照抄参考工程
`Core/Src/stm32f1xx_hal_msp.c:72`），加上后板子进入**复位循环**：

- 实测 IWDG 读回复位默认值 `PR = 0` / `RLR = 0x0FFF`（说明 `MX_IWDG_Init` 从未执行）
- `g_th = 0`（说明 `Control_Apply` 从未执行）
- PC 在启动最开头与最末尾之间乱跳

回退后恢复正常，故确认是**这一行的位置**问题、而不是"分组值本身"问题。

**当前约定**：保持 HAL 默认的 `GROUP_4`（4 位全为抢占优先级），USART3 抢占 6 <
SysTick 抢占 15，方向正确。`main()` 与 `HAL_MspInit()` 里都**刻意不动分组**。

## boot-order

**启动顺序保持最朴素的三步：`HAL_Init() → SystemClock_Config() → 各外设初始化`。**

**踩坑记录**：曾在 `main()` 开头额外加三个"改进"：

- (a) 手工给 `SystemCoreClock` 赋值
- (b) 把 IWDG 提前到时钟配置之前
- (c) 在配置时钟之前就初始化调试串口

结果是**板子彻底毫无反应**；把这三点去掉、回到与参考工程（已在同一块板上跑通）
一致的顺序后就能跑。

- `SystemCoreClock` 的正确初值由已对齐的 `system_stm32f1xx.c` 提供（8000000），
  **不需要也不要在 `main` 里手工赋值**。
- IWDG 提前的后果见 [`#iwdg-start-late`](#iwdg-start-late)；
  调试串口提前的后果是"没有任何输出可看"，让后两个问题的定位彻底失明。

## boot-progress

**启动进度闪灯码（已按需求删除，此处保留恢复方法）。**

`Led0_BlinkMark(n)` 用红灯闪 n 次报"走到第几步"：

| 次数 | 含义 |
| --- | --- |
| 1 | `HAL_Init` |
| 2 | IWDG |
| 3 | 时钟 |
| 4 | LCD |
| 5 | 采集 / 控制 |
| 6 | EEPROM |
| 7 | 通信 |
| 之后 | 转为 5 s 慢心跳（主循环已启动） |

**为什么当时需要它**：板载串口打不出任何字节、某步初始化卡死时板子就是"毫无反应"，
肉眼无法区分卡在哪；闪灯码不需要任何额外工具即可读出卡点。

**为什么删掉**：3~7 共 25 次闪烁使开机超过 25 秒，严重拖慢每轮验证与演示；
而它当初要定位的"板子毫无反应"根因（半主机 `bkpt`，见 [`#semihosting`](#semihosting)）
已解决。现在只保留末尾一次 `Led0_BlinkMark(2)` 作"启动完成"提示。

**若日后要恢复**：`Led0_BlinkMark(n)` 本身**保留在 `main.c` 里**（它仍是可用的诊断原语），
在对应初始化步骤后加一行调用即可；但**注意总时长** —— 它必须仍然放在 `MX_IWDG_Init()`
之前，且不能让"开狗到主循环首次喂狗"逼近看门狗超时（见
[`#iwdg-start-late`](#iwdg-start-late)）。

## lcd

**面板识别靠读 ID，不靠猜。**

`lcd_show.c` 的 `LcdShow_Init()` 内部调 BSP `lcd_init()`，后者读 LCD 控制器 ID 并据此
设置宽高与扫描方向，然后由本项目打印一行自证：

```
LCD ID:7796
[lcd] id=0x7796 width=480 height=320 dir=1
```

- `0x7796` = **ILI9486/9488 系 4.0" 480×320**（正点原子精英版 V2 板载屏）
- 若这一行变成 `id=0x0000 width=0 height=0`，**先怀疑测量方式**（见
  [`#debug-only-once`](#debug-only-once)：`hotplug` 读 RAM 读到的可能是复位瞬间的 `.bss`），
  再怀疑硬件。

**"黑屏 ↔ 白屏"交替闪烁的定性方法**：这种现象说明**程序在反复重启**
（每次重启 LCD 重新初始化 → 背光/清屏状态在两种状态间跳），而不是"屏坏了"或
"时序配错了"。用 [`#reset-forensics`](#reset-forensics) 读 `RCC_CSR` 一击即中：
当时读到的就是 `IWDGRSTF = 1`，直接指向看门狗复位循环。

**为什么不能改 BSP**：`Drivers/BSP/LCD/lcd.c` / `lcd.h` / `lcd_ex.c` / `lcdfont.h`
与参考工程**逐字节相同**且是 **GBK 编码**；所有 LCD 相关改动都留在
`Core/Src/lcd_show.c`。（AGENTS.md「已知陷阱」#26 是速查版。）
