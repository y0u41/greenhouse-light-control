# 固件踩坑全记录（lessons）

> **本文件与 [`AGENTS.md`](../AGENTS.md) 是什么关系？**
>
> - [`AGENTS.md`](../AGENTS.md) 是**入口文件**：只讲规则红线、文档地图，以及"动手前必读"的
>   3~6 条最危险的坑。它**不承载踩坑细节**（篇幅有限，写长了就没人读完）。
> - **本文件是全部踩坑的唯一详版**：现象 → 观测证据 → 根因 → 修法 → 复现方法。
> - 原 `AGENTS.md`「已知陷阱」速查表（常规 1–13 条 + 连续 1–27 条）**已整体并入本文件**，
>   不再有两份。文档里的旧**编号**引用已全部改成直接指向本文件锚点
>   （如 `#actuator`、`#ascii-path`）；下文保留**编号对照表**仅作历史映射，
>   供追溯"当年第 N 条"的来历，日常引用请直接用锚点。
> - `main.c` / `main.h` 的注释按 `docs/lessons.md#<锚点>` 指到这里；**锚点名不可随意改名**。

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
| [`#light-sensor`](#light-sensor) | 光敏是 PF8/ADC3_IN6（官方 readme 误写 PA1）与 `light = 4095 - adc` |
| [`#actuator`](#actuator) | 执行器引脚主权（PD11 / FSMC 占用脚）、`ACTUATOR_*` 抽象与两阶段 |
| [`#ports`](#ports) | 端口纪律：llama-server 8092 与 Qt NodeServer 9092 必须错开 |
| [`#beep-led`](#beep-led) | 蜂鸣器 PB8 高电平响；LED0/LED1 语义分工 |
| [`#tts`](#tts) | TTS 语音名（无 `Microsoft Xiaoxiao`）与 TextToSpeech 模块缺失 |
| [`#ascii-path`](#ascii-path) | 源码路径含中文导致 Qt AutoMoc 失败 |
| [`#keil-project`](#keil-project) | Keil 工程由脚本生成（uvoptx 调试器 / 模板缺 HAL 文件） |
| [`#bom`](#bom) | 含中文的 `.ps1` 必须保持 UTF-8 **with BOM** |
| [`#esp8266-at`](#esp8266-at) | ESP8266 AT 时序四坑（异步 CIPSTART / CIPMODE 顺序 / 重烧不复位 / `+++`） |
| [`#toolchain-hygiene`](#toolchain-hygiene) | 工具链卫生：烧录与调试的替代路径、AC6 隐式声明、DTR/RTS、`DEBUG_UART` |
| [`#eeprom-ack-poll`](#eeprom-ack-poll) | I2C ACK 轮询必须有探测间隔，否则写周期内整轮耗尽误报失败 |
| [`#code-invariants`](#code-invariants) | 代码不变式：失败不致命、单一事实源、行装配器复位、cJSON 堆预算 |
| [`#attic`](#attic) | 工程卫生地雷：`_attic/`、`lcd_ex.c`、`_template_*.uvoptx` |

---

## 原「AGENTS.md 已知陷阱」编号对照

`AGENTS.md` 收缩为入口文件后，「已知陷阱」速查表已并入本文件。原编号保留了**两套**
（这是历史遗留的不一致，本次一并说明）：开头「常规」表编号 **1–13**，随后
「上板调试 / 架构约束 / 工程卫生」是**连续编号 1–27**（上板 1–16、架构 17–23、卫生 24–27）。

| 原编号 | 主题 | 现锚点 |
| --- | --- | --- |
| 常规 #1 | 光敏是 PF8 = ADC3_IN6，不是 PA1（官方 readme 笔误） | [`#light-sensor`](#light-sensor) |
| 常规 #2 | 光照越强 `adc` 越小；`light = 4095 - adc`；不要用 `lsens_get_val()` | [`#light-sensor`](#light-sensor) |
| 常规 #3 | 执行器 PD11 不得放 PD5；FSMC 占用脚；两阶段；`ACTUATOR_*` 抽象 | [`#actuator`](#actuator) |
| 常规 #4 | Qt 监听 9092 / llama-server 8092 必须错开 | [`#ports`](#ports) |
| 常规 #5 | 蜂鸣器 PB8 高电平响 | [`#beep-led`](#beep-led) |
| 常规 #6 | TTS 语音名易错 + TextToSpeech 模块未装 | [`#tts`](#tts) |
| 常规 #7 | LED0 = 心跳 / LED1 = 执行器指示，不得混用 | [`#beep-led`](#beep-led) |
| 常规 #8 | 源码路径含中文会让 Qt moc 失败 | [`#ascii-path`](#ascii-path) |
| 常规 #9 | Keil 工程不要手改，用脚本生成 | [`#keil-project`](#keil-project) |
| 常规 #10 | `tools/build.ps1` 必须保持 UTF-8 with BOM | [`#bom`](#bom) |
| 常规 #11 | 只给 `.uvprojx` 会导致烧录失败（调试器在 `.uvoptx`） | [`#keil-project`](#keil-project) |
| 常规 #12 | 未关闭半主机（semihosting）会死在 `main()` 之前 | [`#semihosting`](#semihosting) |
| 常规 #13 | IWDG 必须在全部开机耗时步骤之后才启动 | [`#iwdg-start-late`](#iwdg-start-late) |
| 连续 #1 | 半主机（同上） | [`#semihosting`](#semihosting) |
| 连续 #2 | `IWDG_RLR` 只有 12 位，重装值永远 ≤ 4095 | [`#iwdg-reload-12bit`](#iwdg-reload-12bit) |
| 连续 #3 | F1 的 IWDG 一旦启动，系统复位不清除 | [`#iwdg-survives-reset`](#iwdg-survives-reset) |
| 连续 #4 | 写完 `IWDG_PR/RLR` 必须轮询 `IWDG_SR`，不要裸寄存器直写 | [`#iwdg-sr-poll`](#iwdg-sr-poll) |
| 连续 #5 | `HAL_IWDG_Refresh()` 空句柄会写地址 0 | [`#iwdg-refresh-null`](#iwdg-refresh-null) |
| 连续 #6–#9 | ESP8266 AT 时序四坑 | [`#esp8266-at`](#esp8266-at) |
| 连续 #10–#12 | ST-Link `hotplug` 复位目标 / pwsh 别名吞命令 / 2 字节对齐读错 | [`#debug-only-once`](#debug-only-once) |
| 连续 #13 | AC6 把隐式函数声明当错误 | [`#toolchain-hygiene`](#toolchain-hygiene) |
| 连续 #14 | 串口工具 DTR/RTS 组合会按住复位 | [`#toolchain-hygiene`](#toolchain-hygiene) |
| 连续 #15 | `DEBUG_UART=0` 时串口一个字节都没有 | [`#toolchain-hygiene`](#toolchain-hygiene) |
| 连续 #16 | 复位原因取证：`RCC->CSR` 粘滞标志 | [`#reset-forensics`](#reset-forensics) |
| 连续 #17 | 业务模块一律返回状态码，绝不调 `Error_Handler()` | [`#code-invariants`](#code-invariants) |
| 连续 #18 | 采样失败不能与"极值"共用返回值 | [`#code-invariants`](#code-invariants) |
| 连续 #19 | 断链/重连必须复位协议层行装配器 | [`#code-invariants`](#code-invariants) |
| 连续 #20 | cJSON 是 malloc 型解析器，堆预算必须留足 | [`#code-invariants`](#code-invariants) |
| 连续 #21 | `IT` 侧 relay 与 buzz 已解耦 | [`#code-invariants`](#code-invariants) |
| 连续 #22 | 配置宏的单一事实源 | [`#code-invariants`](#code-invariants) |
| 连续 #23 | `#if DEBUG_UART` 不要写成 `#ifdef` | [`#code-invariants`](#code-invariants) |
| 连续 #24 | 地雷文件已移出 `firmware/` 树到 `_attic/` | [`#attic`](#attic) |
| 连续 #25 | `Drivers/BSP/LCD/lcd_ex.c` 绝不能加入工程 | [`#attic`](#attic) |
| 连续 #26 | `lcd.c/h`、`lcd_ex.c`、`lcdfont.h` 逐字节相同且是 GBK | [`#lcd`](#lcd) |
| 连续 #27 | `_template_atk_f103.uvoptx` 不要删 | [`#keil-project`](#keil-project) |

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

## iwdg

IWDG 是本项目**最容易做出"板子毫无反应"**的外设，下面 6 条是实测踩出来的，
顺序按"配置一只正确的狗"的操作次序排。

### iwdg-reload-12bit —— 重装值必须 ≤ 4095

<a id="iwdg-reload-12bit"></a>

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

### iwdg-survives-reset —— 启动后系统复位不清它

<a id="iwdg-survives-reset"></a>

**STM32F1 的 IWDG 一旦启动，系统复位（含按键复位、`NVIC_SystemReset`）不会清除它**
（只有上电复位 POR 会）。所以配置错了就是**永久复位循环**，每一轮沿用同一份错配置 ——
这解释了为什么"反复烧录 / 反复按复位键完全没用"。

**操作含义**：**改狗参数（PR / RLR）后必须整板断电重新上电再验证**，光按复位键不算验证。

### iwdg-sr-poll —— 写完 PR/RLR 必须轮询 IWDG_SR

<a id="iwdg-sr-poll"></a>

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

### iwdg-refresh-null —— 喂狗用裸写，不要用 HAL_IWDG_Refresh

<a id="iwdg-refresh-null"></a>

**踩坑记录**：`HAL_IWDG_Refresh(&s_hiwdg)` 在 `s_hiwdg.Instance == NULL` 时会写地址 0。
展开后就是：

```c
WRITE_REG(hiwdg->Instance->KR, 0xAAAA)   /* Instance == NULL -> 往 0x00000000 写 */
```

**实测现场**：`CFSR.IMPRECISERR = 1`、`HFSR.FORCED = 1`，异常栈帧里
`R2 = 0x0000AAAA`（正是要写的键值）。

**修法**：喂狗用裸写 `IWDG->KR = 0xAAAAU;`（不依赖句柄）。参考工程同样用裸写。

### iwdg-dbgmcu-freeze —— 调试时冻结看门狗

<a id="iwdg-dbgmcu-freeze"></a>

`DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP`（照抄参考工程 `User/App/app_main.c:132`）
让**调试器暂停 CPU 时冻结看门狗**。

**不加会发生什么**：在 Keil 里一下断点 CPU 就停止喂狗，**≈6.4 s** 后必被复位 ——
现象是"一进断点就重启、完全没法单步调试"。

**注意**：该宏在部分 CMSIS 头版本里没有定义，故 `main.c` 里自带
`#ifndef DBGMCU_CR_DBG_IWDG_STOP / #define 0x00000100U` 兜底。

### iwdg-start-late —— 启动放在所有耗时步骤之后

<a id="iwdg-start-late"></a>

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
`Core/Src/lcd_show.c`。（原 AGENTS.md「已知陷阱」连续 #26。）

## light-sensor

**光敏传感器是 PF8 = ADC3_IN6，不是 PA1。**

- 依据官方 `Drivers/BSP/LSENS/lsens.h`：`LSENS_ADC3_CHX_GPIO_PIN = GPIO_PIN_8`（GPIOF）、
  `LSENS_ADC3_CHX = ADC_CHANNEL_6`
- 官方 `实验20 光敏传感器实验/readme.txt` 的"实验现象"段把它误写成 `通道6(PA1)` ——
  **那是官方笔误**，不要采信
- 精英版 V2 上 PA1 是电容触摸按键 TPAD / 电位器 RV1（经 P7 跳线），与光照无关；
  官方 `adc3_init()` 顺带把 PA1 配成模拟输入是无害副作用（高阻），**建议拔掉 P7 跳线**
- 另注：官方 `adc3.h` 里的 `ADC3_CHY_GPIO_PIN = GPIO_PIN_1` / `ADC3_CHY = ADC_CHANNEL_1`
  是**残留默认值**，与光敏无关；**不要为了"改通道"去动 `adc.h`**（那是 ADC1/ADC2 的定义）
- 官方 `adc3_init()` **只配 PA1、不会配 PF8**；PF8 的模拟输入模式由官方 `lsens_init()` 负责。
  本项目在 `LightSensor_Init()` 里自行完成 `__HAL_RCC_GPIOF_CLK_ENABLE()` + PF8 的
  `GPIO_MODE_ANALOG` + `ADC_CHANNEL_6` + `HAL_ADCEx_Calibration_Start()`，不依赖官方 `adc3.c`

**光照越强，ADC 原始码值越小。**

- 官方 `lsens.c`：`return (uint8_t)(100 - adc3_get_result_average(...) / 40)`，注释为
  `0,最暗; 100,最亮` → 亮时原始 ADC ≈ 0
- 因此统一改用 `light = 4095 - adc` 作为光强指标，**阈值判断只写在 `light` 上**
- **不要复用 `lsens_get_val()`**：它把 4096 级压成 100 级（分辨率 2.44%），而协议 `adc`
  声明的是 0–4095。要取原始值请用 `adc3_get_result_average(ADC_CHANNEL_6, n)`
- 采样失败**不要**返回 0（那是"最强光"）——哨兵与门控见 [`#code-invariants`](#code-invariants)

> 引脚与阈值语义的唯一事实来源是 Tech_Design.md 附录 A.1 / A.2。

## actuator

**执行器目标脚是 PD11，绝不能放 PD5；阶段一先用板载 LED1 等效。**

- PD5 = `FSMC_NWE`（TFTLCD 写选通）。插屏后每写一次屏 PD5 就翻转，
  执行器会**高频乱跳并破坏 LCD 时序**
- 板载 FSMC 占用：PD0/PD1/PD4/PD5/PD8~PD10/PD14/PD15、PE7~PE15、PG0/PG12、PB0(背光)
  —— **这些脚一律不得挪用**
- PD11 板载未接任何器件，是外接继电器模块的目标脚，**必须预留**：该位置的排针/焊盘
  不得接其他功能
- **继电器借用渠道已确认，但尚未到手**，因此采用两阶段验证：
  - **阶段一（先验实验）**：板载 LED1（PE5）作等效负载，先跑通全部控制逻辑
  - **阶段二（正式验证与验收）**：借用件到位后换成 PD11，用真实继电器复测与演示
- 执行器的端口/引脚/时钟/名称必须集中在 `ACTUATOR_GPIO_PORT` / `ACTUATOR_GPIO_PIN` /
  `ACTUATOR_CLK_ENABLE` / `ACTUATOR_NAME` 四行宏里，业务代码只调 `Actuator_Init()` /
  `Actuator_Set()`，**不得直接出现 `GPIOE` / `GPIO_PIN_5` / `GPIOD` / `GPIO_PIN_11`**；
  阶段切换时只换这 4 行宏
- **保留继电器接口空间**（除引脚外）：独立 5 V 与 GND 各一路通路至继电器位置、
  模块安放位置（约 50 × 25 mm），一并不得挪作他用
- 板载 LED 与推荐的低电平触发继电器模块**都是低电平有效**，
  故 `ACTUATOR_ACTIVE_LEVEL = 0` 在两种配置下通用
- 成品继电器模块**内部已自带驱动三极管、光耦与续流二极管**，接 VCC / GND / IN 三线即可，
  **不要额外加 S8050 / 1N4007**；仅裸继电器才需要外接驱动元件
- 优先选**低电平触发**模块：3.3 V 灌电流可靠；高电平触发时 3.3 V 仅约 2.3 mA，
  可能推不饱光耦

> 完整配置见 Tech_Design.md 附录 A.3 / A.5 / A.6。

## ports

**Qt 监听 9092，llama-server 占用 8092，两者必须错开。**

- `D:\llama.cpp\模型启动\启动.cmd` 用 `netstat :8092 LISTENING` 判断服务是否在跑；
  若 Qt 占了 8092，该脚本会误报"服务已在运行"
- 节点 `AT+CIPSTART` 的目标是 `<PC_IP>:9092`；PC 建议静态 IP 或 DHCP 保留，
  并**放行 9092 入站**（Qt 已绑 `0.0.0.0`，缺的往往是防火墙规则）
- llama-server 服务管理**脚本清单与日志位置**见 `README.md` §4（llama.cpp）
- 服务以 **router 模式**运行（`--models-dir --models-max 1`）：模型按需加载、
  同一时刻仅驻留一个。请求体的 `model` 字段取值必须来自 `GET /v1/models`；
  **首次请求 / 切换模型要付权重加载时间**，演示前必须先预热一次；
  若不需要切模型，直接用固定单模型启动（`-m glm-4.7-flash.gguf`）
- **别的 TCP 服务端会抢 9092**：一旦有第二个进程监听 9092（如硬件在环探针），
  节点就连到"错误的那一个"服务端上，表现为 Qt 收不到帧、或收到一半的会话。
  工具的用法与前提见 `README.md` §硬件在环验证

## beep-led

**蜂鸣器 PB8 是"高电平响"，且上电必须为关。**

- 官方 `beep.c` 的 `beep_init()` 末尾是 `BEEP(0)`（关闭），即 `GPIO_PIN_SET` 才是鸣响
- 板载蜂鸣器高电平响，初始化遗漏会让它**上电就一直叫**；
  `beep_init()` 必须先于业务逻辑调用

**两颗板载 LED 分工固定，不要混用。**

- LED0（PB5，红）= **运行心跳**，每 **5 s** 翻转一次（5 s 亮 / 5 s 灭）
- LED1（PE5，绿）= **执行器指示**，由执行器抽象驱动
- 板载 LED 是**低电平点亮**（官方 `led.c`：`LED0(1)` / `LED1(1)` 均为关闭）
- 不要让同一颗灯同时承担"程序活着"和"执行器吸合"两种语义，
  否则演示时无法判断灯亮的原因

## tts

**TTS 语音名容易写错，且 Qt TextToSpeech 模块本机未装。**

- **`Microsoft Xiaoxiao` 本机不存在**（它是"讲述人"的自然语音，SAPI/TextToSpeech 取不到），
  不要把它写成默认值
- 本机可用的中文语音：SAPI5 仅 `Microsoft Huihui Desktop`；OneCore 另有
  Huihui / Kangkang / Yaoyao
- 实现要求：`availableVoices()` 枚举 + 按 `zh-CN` 过滤 + 回退，不要硬编码
- 前置条件：Qt 6.12.0 MinGW 套件当前**未安装 Qt TextToSpeech 模块**，
  需用 MaintenanceTool 补装后才能编译播报功能（未装时 CMake 自动探测并关闭播报，
  其余功能不受影响）

## ascii-path

**源码路径含中文会让 Qt 的 moc 失败（本仓库路径就是中文）。**

- 现象：`AutoMoc: moc: Cannot create <...>/moc_xxx.cpp. Error: No such file or directory`，
  随后 `mocs_compilation.cpp` 里 `#include "xxx/moc_yyy.cpp"` 找不到文件
- 原因：Qt 的 AutoMoc 无法把生成文件写入非 ASCII 路径；**不要**试图用 8.3 短路径绕过
  —— 本机短路径仍是中文（`D:\正点原子\个人项~1\智能温~1`）
- **正确做法**：用 `tools/build.ps1`（自动建 `D:\glc -> 仓库根` 的 ASCII 联接后构建），
  或手工建立同类联接 / 把工程放到纯 ASCII 路径
- `build.ps1` 同时负责探测 Keil（`D:\MDK5.36`）与 Qt（`E:\Qt`）并注入 PATH

## keil-project

**Keil 工程不要手改，用脚本生成。**

- 官方模板缺 `stm32f1xx_hal_i2c.c`（AT24C02）与 `stm32f1xx_hal_iwdg.c`（看门狗），
  **手写工程必漏**
- 增删源文件后跑 `py tools/make_keil_project.py` 重新生成 `greenhouse.uvprojx`；
  `--check` 只做引用校验

**只给 `.uvprojx` 会导致烧录失败 —— 调试器选择在 `.uvoptx` 里。**

- 现象：`UV4 -f` 报 `Internal DLL Error` + `Flash Download failed - Target DLL has been cancelled`
- 原因：**调试器选择不属于工程（uvprojx），只存在于用户选项（uvoptx）**。缺少 uvoptx 时
  Keil 回退到官方模板的默认调试器 = ULINK2（`BIN\UL2CM3.DLL`），本机没有 ULINK 设备
  → 下载被取消
- 本机是 **ST-Link**，对应 `nTsel=6` + `pMon=STLink\ST-LINKIII-KEIL_SWO.dll`
  （该 DLL 随 Keil 提供：`D:\MDK5.36\ARM\STLink\`）
- `tools/make_keil_project.py` 会**同时生成 `greenhouse.uvoptx`**（基于
  `_template_atk_f103.uvoptx`，锁定为 ST-Link），所以正常流程不该再遇到此问题；
  若手工只放了 uvprojx，请重跑该脚本
- **`_template_atk_f103.uvoptx` 不要删** —— 脚本需要它才能生成带 ST-Link 配置的
  `greenhouse.uvoptx`；缺失时新克隆会退回 ULINK2，报 `Internal DLL Error` /
  `Flash Download failed`
- 成功的烧录日志长这样：
  `Erase Done. Programming Done. Verify OK. Application running ...`

## bom

**`tools/build.ps1` 含中文，必须保持 "UTF-8 with BOM"。**

- 现象：脚本突然报 `Unexpected token` / `Missing closing '}'` / 字符串被截断
- 原因：Windows PowerShell 5.1 把**无 BOM** 的 UTF-8 当 ANSI 读。用文本工具
  （含 AI 写文件）重写该文件会**静默丢掉 BOM**
- 修复一行：

  ```powershell
  $p='tools\build.ps1'; $t=[IO.File]::ReadAllText($p,[Text.Encoding]::UTF8)
  [IO.File]::WriteAllText($p,$t,(New-Object Text.UTF8Encoding($true)))
  ```

- 同理适用于任何含中文的 `.ps1`（`.md` 与 `.py` 不受影响，它们按 UTF-8 读）

## esp8266-at

**ESP8266 AT 时序四坑（每一条都实测踩过）。**

1. **`AT+CIPSTART` 是异步的**：先回 `OK`（命令已接受），再回 `CONNECT`（真正建链）。
   **只有 `CONNECT` / `ALREADY CONNECTED` 才算连上**，把 `OK` 当成功会得到
   "板子自认已连接、PC 侧却没有 Established"的假象。
2. **`AT+CIPMODE=1`（透传）必须在 `AT+CIPSTART` 之前设**，顺序是
   `CIPMUX=0 → CIPMODE=1 → CIPSTART → CONNECT → CIPSEND → OK → '>'`。
   先 CIPSTART 再 CIPMODE=1 时，CIPSTART 能拿到 CONNECT、CIPMODE 也回 OK，
   但紧接着的 `AT+CIPSEND` 会直接回 **`ERROR`**，永远进不了透传。
   （原因：CIPMODE 必须在建链时就确定，建链之后再切不会让模块重新进入透传。）
   > （此顺序现已在 `Tech_Design.md` 的 AT 状态机 FACT 块、其后的说明段与
   > `esp8266.c` 文件头注释中**全部统一**，不要再按"先 CIPSTART"改回去；
   > `ESP_CIPMODE` 状态已不再使用，保留枚举只为兼容。）
3. **重烧 MCU 不会复位 ESP8266 模块**（模块由整板供电，只有整板断电才复位）。
   上一次若停在透传态，模块会**不解释任何 AT 命令**（把它们当 TCP 数据发出去），
   表现为 `AT+RST` 永远等不到 `ready` → 无限退避、`net` 恒为 0。
   解法：`ESP_RST` 先发**裸 `+++`**（前后各需 ≥20 ms 静默）再发裸 `CRLF` 清残行，
   最后才 `AT+RST`。
4. **绝不能用 `at_send("+++")`** —— 该函数会补 `\r\n`，而 `+++` 转义序列
   **不能带 CR/LF**，带了就失效。必须用裸 `esp_uart_write()`。

## toolchain-hygiene

**烧录 / 调试不必依赖 Keil —— ST 官方工具链已在 VS Code 扩展里解压就绪。**

- 位置：`%LOCALAPPDATA%\stm32cube\bundles\`
- `STM32_Programmer_CLI.exe`（`programmer\2.23.0\bin\`）——**绕开 Keil 的 ST-Link DLL**，
  Keil 报 `ST-LINK USB communication error` / `Target DLL has been cancelled` 时的可靠替代：
  - 烧录校验：`-c port=SWD mode=normal -w <hex> -v -rst`
  - 读内核寄存器：`-c port=SWD mode=hotplug -coreReg`（**hotplug 不复位**，能抓真实现场）
  - 读内存：`-c port=SWD mode=hotplug -r32 <addr> <size>`（**`<size>` 不可省略**）
  - 读复位原因：`-r32 0x40021024 0x04`（`RCC_CSR`：`IWDGRSTF` 等标志位）
- `ST-LINK_gdbserver.exe`（`stlink-gdbserver\7.14.0+st.2\bin\`）+ `arm-none-eabi-gdb.exe`
  （`gnu-gdb-for-stm32\`）可做断点级调试。**两个坑**：
  (a) gdb 连接时会自动复位目标；
  (b) `break <符号名>` 可能命中启动文件里的**弱定义** —— `HardFault_Handler` 会解析到
  `0x08000242` 而非向量表里的 `0x0800551C`。**定位异常请用绝对地址**：`break *0x0800551c`
- `arm-none-eabi-gcc / objdump / nm / addr2line` 14.3.1 位于
  `gnu-tools-for-stm32\14.3.1+st.2\bin\`
- ⚠ `mode=hotplug` 有复位目标的风险，读数方法与坑见 [`#debug-only-once`](#debug-only-once)

**其他工具链卫生。**

- **AC6 把隐式函数声明当错误**（`-Wimplicit-function-declaration`）。新增跨模块调用后
  **必须补对应 `#include`**，否则直接编译失败。
  本次 `Light_IsValid()` / `Dbg_Printf()` / `Protocol_Reset()` 各踩一次。
- **串口工具的 DTR/RTS 组合决定板子是否被按在复位上。** 实测可用组合是
  **`DTR=False / RTS=False`**；另外三种（含 T/T）收到 0 字节。组合不对时表现为
  "串口只有几个乱码字节"或"开着串口板子不启动、一关就启动"。
- **`DEBUG_UART=0` 时串口一个字节都不会有**（`Dbg_Init` 是空实现，`USART1` 未初始化）。
  上板诊断前先确认 Keil 工程 `<Define>` 里有 `DEBUG_UART=1`。

## eeprom-ack-poll

**I2C ACK 轮询必须在探测之间留间隔，否则整轮会在写周期开始的瞬间耗尽。**

- 现场现象（N-1）：`set_th` 回 `err` 并回滚，串口实捕
  `[cfg] EEPROM_Save FAILED for th=503 -> rolled back to 2000`；
  但逻辑分析仪显示 0x50 应答 ACK、位翻转探测也有 ACK —— "器件明明在"。
- 根因（主机测试 `test_eeprom.c` 的 `write_cycle_busy_then_ack` 用例复现）：
  AT24C02 页写入后进入 **≤5 ms 内部写周期**，期间对**地址探测**一律 NACK；
  而 `HAL_I2C_IsDeviceReady` 在探测到 NACK 时**立即**返回 `HAL_ERROR` ——
  它的 `Timeout` 参数只约束"等总线标志位"的时间，**不是两次探测之间的间隔**。
  旧代码连续 20 次探测之间不加任何等待，整轮在 **<<1 ms** 内耗尽，
  于是即使数据其实已经写进去，`EEPROM_Save` 也**误报失败**。
- 修法：每次探测后 `HAL_Delay(1)`（10 次 × 1 ms = 10 ms 窗口 ≥ 5 ms 写周期，
  总耗时远小于 IWDG 超时；每 5 次探测喂一次狗）。附带回读校验与
  I2C 外设锁死恢复（`HAL_I2C_DeInit` + 重 Init 后重试一次）。
- 复现/回归方法：`py tests/host/run_host_tests.py`（eeprom 套件）。
  伪 AT24C02 把"写周期忙"建模为 `tick < write_busy_until` 时探测/读立即 NACK，
  `HAL_Delay` 推进 tick —— 没有间隔的旧轮询在 tick 不变时必然耗尽 → 用例红。

## code-invariants

**驱动失败不得致命化（架构性约束）。**

- 业务模块一律返回状态码，**绝不调用 `Error_Handler()`**。
  `Error_Handler()` 是 `__disable_irq() + for(;;)`；而 `MX_IWDG_Init()` 在 `main()` 末尾，
  `adc_light.c` / `eeprom.c` / `esp8266.c` 的初始化都**在它之前** → 触发即
  **永久挂死且连复位都没有**（狗还没开）。
- 参考工程把 `Error_Handler` 的调用**限制在 CubeMX 生成的 `Core/` 文件里**，
  `User/App/**` 与 `Drivers/BSP/**` 一次都没有 —— 这就是它守住的那条边界。

**采样失败不能与"极值"共用同一个返回值。**

- `Light_ReadRaw` 原来失败时 `return 0`，而 `LIGHT_FROM_ADC(0) = 4095 = 最强光`
  → "传感器故障"被翻译成"光照过强报警"并驱动执行器。
- 现在：失败时返回哨兵 `ADC_LIGHT_INVALID = 0xFFFF`（12 位量程内不可能出现），
  **保持上次有效值** + `Light_IsValid()` 标志，`Control_Loop()` 在无效时
  **拒绝任何自动决策**（维持现状）。

**断链 / 重连时必须复位协议层的行装配器。**

- 断了半帧后重连，新字节会被追加到旧半帧之后 → 第一条指令必然解析失败且
  **静默丢弃**（上位机只看到 ack 超时）。故有 `Protocol_Reset()`，
  由 `on_link_lost()` 调用（参考工程对应的是 `mqtt_rx_reset()`）。

**cJSON 是 malloc 型解析器，堆预算必须留足。**

- 每字符串 / 每数字 / 每节点都分配，一条典型指令约 300~390 B。
  原来 `Heap_Size = 0x200`（512 B）只够 60~77%，再加几个"未知键"（设计上要忽略）
  就越界 → `malloc` 失败 → `cJSON_Parse` 返回 NULL → **整条指令静默丢弃**。
- 现已提到 `0x1000`（4 KB，约 10 倍余量）。
- **口径统一**：`startup_stm32f103xe.s` 里的 `Heap_Size`（**`0x1000`**）是唯一事实来源；
  `firmware/CUBEMX_SETUP.md` 步骤 ⑤ 已按此值同步（原先的 "`0x800` 以上" 已删除）。

**其他代码不变式。**

- **`IT` 侧的 relay 与 buzz 已解耦**，报警声走 `ALARM_BEEP_ON_MS` /
  `ALARM_BEEP_OFF_MS`（各 10 s）占空比。**不要退回 `g_buzz = g_relay`** ——
  那会让遮阳动作期间蜂鸣器连续长鸣数小时。
- **配置宏的单一事实源**：`LIGHT_ADC_MAX` 在 `main.h`（`adc_light.h` 只保留
  `LIGHT_FROM_ADC`）；`PROTOCOL_FRAME_MAX` 在 `protocol.h`（`protocol.c` 引用它）。
  曾出现同一常量三处副本（`200`/`200`/`200`）与四处硬编码 `4095`，改一处必漏改。
- **`#if DEBUG_UART` 不要写成 `#ifdef DEBUG_UART`** —— `main.h` 里它是
  **无条件定义**的（缺省 0），`#ifdef` 恒为真，语义就反了。
- **Qt 配置用 QSettings**，组织名 `Greenhouse`、应用名 `LightControl`
  （配置项清单见 Tech_Design.md §配置项）。

## attic

**地雷文件已移出 `firmware/` 树到 `_attic/`。**

- 已移出：`Drivers/_bak_cmsis`、`Drivers/_bak_hal`（含 63 个重复 `.c` 与 40+ 组重名
  include guard）、`Drivers/SYSTEM/usart/usart.c`（重复的 `fputc` / `_sys_*` /
  `HAL_UART_RxCpltCallback`）、`Drivers/BSP/{ADC,LED,LSENS}`（`adc.h` 与 `adc3.h` 共用
  `__ADC_H` guard 且 `ADC_ADCX` 一处是 ADC1 一处是 ADC3；`led.h` 的 PE5 与 `control.c`
  的执行器同脚）、`Middlewares/cJSON/cjson_test.c`（自带 `main()`）
- **⚠️ 这些文件一旦被"顺手加进 Keil 工程"就是链接失败或语义冲突。**
- **`usart.h` 必须留在 `Drivers/SYSTEM/usart/`** —— `lcd.c:36` 依赖它（移走会编译失败，
  已实测）。搬移任何文件后**必须重建验证**。
- 基线提交 `9defdeb` 刻意保留了清理前的 `_attic/`，**目的就是让那次删除可完全恢复**：
  `git checkout 9defdeb -- _attic/`（或指定单个子目录）。

**`Drivers/BSP/LCD/lcd_ex.c` 绝不能加入工程。**

- `lcd.c` 已 `#include` 它，再加为独立编译单元会导致 7 个 `lcd_ex_*_reginit` 重复定义。

**`lcd.c` / `lcd.h` / `lcd_ex.c` / `lcdfont.h` 与参考工程逐字节相同**，且是 **GBK 编码**
（`read` 工具会报 `invalid UTF-8 text`，需按 GBK 解码读）。
**所有 LCD 相关修改都应留在 `Core/Src/lcd_show.c`，不要动 BSP。**（详见 [`#lcd`](#lcd)）
