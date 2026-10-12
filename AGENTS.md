# 智能温室感光控制系统 AGENTS 指南

> 本文件写给在本仓库工作的 AI 编码代理与协作者：**先读它，再动手**。
> 文档集版本：**V1.1**（2026-10-12）
> 配套文档：[PRD.md](PRD.md)（需求）、[Tech_Design.md](Tech_Design.md)（技术方案 / 引脚分配唯一事实来源）、[项目规划书.md](项目规划书.md)（对外提交稿）

## 项目概览

- 一个融合 STM32 与本地大模型的温室感光控制：ADC 采集光照、阈值 + 回差自动控制继电器/蜂鸣器、Qt 远程监控、llama.cpp API 语音智能控制
- 技术栈：STM32F103ZET6 精英版 V2（HAL 库）+ ESP8266（AT 固件）+ Qt 6.12.0（Widgets）+ llama.cpp Server
- 当前状态：需求与技术方案已定稿（PRD / Tech_Design / 项目规划书）；**全栈业务代码已落盘**——固件（Core/Src|Inc 下 `adc_light` / `control` / `protocol` / `eeprom` / `esp8266` / `lcd_show` + `main`）、Qt 上位机（`qtapp/src` 全部模块）、辅助脚本（`protocol_schema.py` 协议事实源 / `contract_test.py` 契约测试 / `simulate_node.py` 虚拟节点）均已写入。下一步是把代码落到实板（CubeMX 工程 + Keil/CLion 构建）与 Qt 本机编译联调。**注意：`firmware/` 仅含业务源码，CubeMX 生成的 Core/Src 骨架（如 `main.c` 已有业务版，但 `stm32f1xx_hal_*`、启动文件、`MX_*` 初始化）需由 CubeMX 按 `firmware/CUBEMX_SETUP.md` 生成后并入。**

## 文档地图

| 文件 | 定位 |
| --- | --- |
| `PRD.md` | 需求事实来源，功能范围的唯一依据 |
| `Tech_Design.md` | 技术方案；**引脚分配 / 电平极性 / ADC 与阈值语义的唯一事实来源**（见其附录 A） |
| `项目规划书.md` | 方案论证、硬件选型、开发计划、测试方案（对外提交稿） |
| `项目规划书.docx` | 由 `项目规划书.md` 经 `tools/md2docx.py` 生成，**不要手工改 docx**，改 md 后重新生成 |
| `设计报告.md` / `.docx` | **任务书第 4 项交付物**（设计报告）。实测值与【待测】项严格区分：拿到硬件后按报告 8.3 用例表逐项补测并回填状态列，**禁止把推测值写成实测结果** |
| `AGENTS.md` | 本文件：协作约定与已知陷阱 |
| `docs/lessons.md` | **踩坑全记录**：本文件「已知陷阱」的**完整来龙去脉 + 证据 + 复现方法**（半主机 bkpt、IWDG 六坑、复位源取证、观测工具陷阱、优先级分组、启动顺序、进度闪灯码、LCD 面板识别）。「已知陷阱」是速查条目，本文件是详版，两者刻意不重复。`main.c` 的注释按 `docs/lessons.md#<锚点>` 指到这里 |

- 冲突处理：**PRD > Tech_Design > 项目规划书 > 代码现状**；发现冲突先向用户提出，不要自行选一个默默改掉
- 涉及引脚 / 协议 / 阈值的任何改动，必须**先改 `Tech_Design.md` 附录 A**，再同步其余文档与代码

## 已知陷阱（改代码前必读）

这四条是本仓库最容易踩、且踩了会导致"做不出来 / 演示翻车"的地方，均有开发板官方例程与 IO 分配表作依据：

1. **光敏传感器是 PF8 = ADC3_IN6，不是 PA1**
   - `Drivers/BSP/LSENS/lsens.h`：`LSENS_ADC3_CHX_GPIO_PIN = GPIO_PIN_8`（GPIOF）、`LSENS_ADC3_CHX = ADC_CHANNEL_6`
   - 官方 `实验20 光敏传感器实验/readme.txt` 的"实验现象"段把它误写成 `通道6(PA1)` —— **那是官方笔误**，不要采信
   - 精英版 V2 上 PA1 是电容触摸按键 TPAD / 电位器 RV1（经 P7 跳线），与光照无关
   - 另注：官方 `adc3.h` 里的 `ADC3_CHY_GPIO_PIN = GPIO_PIN_1` / `ADC3_CHY = ADC_CHANNEL_1` 是**残留默认值**，与光敏无关；不要为了"改通道"去动 `adc.h`（那是 ADC1/ADC2 的定义）
2. **光照越强，ADC 原始码值越小**
   - 官方 `lsens.c`：`return (uint8_t)(100 - adc3_get_result_average(...) / 40)`，注释为 `0,最暗; 100,最亮` → 亮时原始 ADC ≈ 0
   - 因此统一改用 `light = 4095 - adc` 作为光强指标，阈值判断只写在 `light` 上
   - **不要复用 `lsens_get_val()`**：它把 4096 级压成 100 级（分辨率 2.44%），而协议 `adc` 声明的是 0–4095。要取原始值请用 `adc3_get_result_average(ADC_CHANNEL_6, n)`
3. **执行器目标脚是 PD11，绝不能放 PD5；阶段一先用板载 LED1 等效**
   - PD5 = `FSMC_NWE`（TFTLCD 写选通）。插屏后每写一次屏 PD5 就翻转，执行器会高频乱跳并破坏 LCD 时序
   - 板载 FSMC 占用：PD0/PD1/PD4/PD5/PD8~PD10/PD14/PD15、PE7~PE15、PG0/PG12、PB0(背光) —— **这些脚一律不得挪用**
   - PD11 板载未接任何器件，是外接继电器模块的目标脚，**必须预留**：该位置的排针/焊盘不得接其他功能
   - **继电器借用渠道已确认，但尚未到手**，因此采用两阶段验证：
     - **阶段一（先验实验）**：板载 LED1（PE5）作等效负载，先跑通全部控制逻辑；
     - **阶段二（正式验证与验收）**：借用件到位后换成 PD11，用真实继电器复测与演示。
   - 执行器的端口/引脚/时钟/名称必须集中在 `ACTUATOR_GPIO_PORT` / `ACTUATOR_GPIO_PIN` / `ACTUATOR_CLK_ENABLE` / `ACTUATOR_NAME` 四行宏里，业务代码只调 `Actuator_Init()` / `Actuator_Set()`，**不得直接出现 `GPIOE` / `GPIO_PIN_5` / `GPIOD` / `GPIO_PIN_11`**；阶段切换时只换这 4 行宏
   - **保留继电器接口空间**（除引脚外）：独立 5 V 与 GND 各一路通路至继电器位置、模块安放位置（约 50 × 25 mm），一并不得挪作他用
   - 板载 LED 与推荐的低电平触发继电器模块**都是低电平有效**，故 `ACTUATOR_ACTIVE_LEVEL = 0` 在两种配置下通用
   - 成品继电器模块**内部已自带驱动三极管、光耦与续流二极管**，接 VCC / GND / IN 三线即可，不要额外加 S8050 / 1N4007；仅裸继电器才需要外接驱动元件
   - 优先选**低电平触发**模块：3.3 V 灌电流可靠；高电平触发时 3.3 V 仅约 2.3 mA，可能推不饱光耦
4. **Qt 监听 9092，llama-server 占用 8092，两者必须错开**
   - `D:\llama.cpp\模型启动\启动.cmd` 用 `netstat :8092 LISTENING` 判断服务是否在跑；若 Qt 占了 8092，该脚本会误报"服务已在运行"
5. **蜂鸣器 PB8 是"高电平响"，且上电必须为关**
   - 官方 `beep.c` 的 `beep_init()` 末尾是 `BEEP(0)`（关闭），即 `GPIO_PIN_SET` 才是鸣响
   - 板载蜂鸣器高电平响，初始化遗漏会让它**上电就一直叫**；`beep_init()` 必须先于业务逻辑调用
6. **TTS 语音名容易写错，且 Qt TextToSpeech 模块本机未装**
   - **`Microsoft Xiaoxiao` 本机不存在**（它是"讲述人"的自然语音，SAPI/TextToSpeech 取不到），不要把它写成默认值
   - 本机可用的中文语音：SAPI5 仅 `Microsoft Huihui Desktop`；OneCore 另有 Huihui / Kangkang / Yaoyao
   - 实现要求：`availableVoices()` 枚举 + 按 `zh-CN` 过滤 + 回退，不要硬编码
   - 前置条件：Qt 6.12.0 MinGW 套件当前**未安装 Qt TextToSpeech 模块**，需用 MaintenanceTool 补装后才能编译播报功能
7. **两颗板载 LED 分工固定，不要混用**
   - LED0（PB5，红）= **运行心跳**，每 **5 s** 翻转一次（5 s 亮 / 5 s 灭）
   - LED1（PE5，绿）= **执行器指示**，由执行器抽象驱动
   - 板载 LED 是**低电平点亮**（官方 `led.c`：`LED0(1)` / `LED1(1)` 均为关闭）
   - 不要让同一颗灯同时承担"程序活着"和"执行器吸合"两种语义，否则演示时无法判断灯亮的原因
8. **源码路径含中文会让 Qt 的 moc 失败（本仓库路径就是中文）**
   - 现象：`AutoMoc: moc: Cannot create <...>/moc_xxx.cpp. Error: No such file or directory`，随后 `mocs_compilation.cpp` 里 `#include "xxx/moc_yyy.cpp"` 找不到文件
   - 原因：Qt 的 AutoMoc 无法把生成文件写入非 ASCII 路径；**不要**试图用 8.3 短路径绕过——本机短路径仍是中文（`D:\正点原子\个人项~1\智能温~1`）
   - **正确做法**：用 `tools/build.ps1`（自动建 `D:\glc -> 仓库根` 的 ASCII 联接后构建），或手工建立同类联接 / 把工程放到纯 ASCII 路径
   - `build.ps1` 同时负责探测 Keil（`D:\MDK5.36`）与 Qt（`E:\Qt`）并注入 PATH
9. **Keil 工程不要手改，用脚本生成**
   - 官方模板缺 `stm32f1xx_hal_i2c.c`（AT24C02）与 `stm32f1xx_hal_iwdg.c`（看门狗），手写工程必漏
   - 增删源文件后跑 `py tools/make_keil_project.py` 重新生成 `greenhouse.uvprojx`；`--check` 只做引用校验
10. **`tools/build.ps1` 含中文，必须保持 "UTF-8 with BOM"**
   - 现象：脚本突然报 `Unexpected token` / `Missing closing '}'` / 字符串被截断
   - 原因：Windows PowerShell 5.1 把**无 BOM** 的 UTF-8 当 ANSI 读。用文本工具（含 AI 写文件）重写该文件会**静默丢掉 BOM**
   - 修复一行：
     ```powershell
     $p='tools\build.ps1'; $t=[IO.File]::ReadAllText($p,[Text.Encoding]::UTF8)
     [IO.File]::WriteAllText($p,$t,(New-Object Text.UTF8Encoding($true)))
     ```
   - 同理适用于任何含中文的 `.ps1`（`.md` 与 `.py` 不受影响，它们按 UTF-8 读）
11. **只给 `.uvprojx` 会导致烧录失败——调试器选择在 `.uvoptx` 里**
   - 现象：`UV4 -f` 报 `Internal DLL Error` + `Flash Download failed - Target DLL has been cancelled`
   - 原因：**调试器选择不属于工程（uvprojx），只存在于用户选项（uvoptx）**。缺少 uvoptx 时 Keil 回退到官方模板的默认调试器 = ULINK2（`BIN\UL2CM3.DLL`），本机没有 ULINK 设备 → 下载被取消
   - 本机是 **ST-Link**，对应 `nTsel=6` + `pMon=STLink\ST-LINKIII-KEIL_SWO.dll`（该 DLL 随 Keil 提供：`D:\MDK5.36\ARM\STLink\`）
   - `tools/make_keil_project.py` 会**同时生成 `greenhouse.uvoptx`**（基于 `_template_atk_f103.uvoptx`，锁定为 ST-Link），所以正常流程不该再遇到此问题；若手工只放了 uvprojx，请重跑该脚本
   - 成功的烧录日志长这样：`Erase Done. Programming Done. Verify OK. Application running ...`
12. **未关闭半主机（semihosting）会让固件在进 `main()` 之前就 HardFault 死掉**（血泪教训，务必先读）
   - 现象：烧录 `Verify OK`，但板子"毫无反应"——LCD 不亮、串口 **0 字节**、连启动闪灯码都不出。
     极易被误判成供电/时钟/接线/屏幕问题
   - 原因：工程链接的是 ARM **完整标准 C 库**（`Define` 里没有 `__MICROLIB`）。标准库在
     `__rt_lib_init -> _initio` 阶段调用 `_sys_open` 打开标准流，而库自带的 `_sys_open`
     走**半主机**通道，底层指令就是 `bkpt 0x00AB`。**没有调试器时 Cortex-M3 执行 BKPT 会直接
     触发 HardFault**，而 `HardFault_Handler` 是 `B .` 死循环 → 永远到不了 `main()`
   - 实测证据：`PC = 0x0800551C (HardFault_Handler)`、`XPSR.IPSR = 3`；异常栈帧 `@0x20001000`
     中 `PC = 0x0800164C`、`LR = 0x08001647`，而 `0x0800164C` 反汇编正是 `bkpt 0x00ab`（在 `_sys_open` 内）
   - 修法：**`firmware/Core/Src/retarget.c` 不可删**（`__use_no_semihosting` + 自行实现全部
     `_sys_*`）。参考工程对应文件是 `Drivers/SYSTEM/usart/usart_retarget.c`
   - 自检（改完必做）：`arm-none-eabi-objdump -d Output/greenhouse.axf | findstr bkpt` **必须无输出**
   - ⚠ 排查陷阱：**接上调试器时 `bkpt` 会被调试器接管**，现象与裸跑完全不同（HardFault 断点反而
     不命中、`continue` 一直不返回）。**不要用"接调试器能跑"来否定这个原因**
13. **IWDG 必须在全部开机耗时步骤之后才启动**
   - 现象：固件反复重启，永远进不了主循环（表现为"板子毫无反应"或反复闪启动码）
   - 原因：IWDG 超时约 **2.0 s**，而开机路径上 `Led0_BlinkMark(6)` 单项就约 2.4 s、
     `Esp8266_Init()` 的 AT 探测更久，且 `Iwdg_Feed()` 直到主循环第一行才被调用
   - 实测证据：`RCC_CSR = 0x3C000002`，其中 **`IWDGRSTF`(bit29) = 1**；
     `IWDG_RLR = 0x4E2 (1250)`、`IWDG_PR = 4 (÷64)` → 1250×64/40000 ≈ 2.0 s
   - 规则：`MX_IWDG_Init()` 只能紧邻 `for(;;)` 之前；开机路径上任何 >1.5 s 的步骤都要重新审视顺序
   - ⚠ **当前配置已不是 2.0 s**：`Reload = 4000U`、`IWDG_PRESCALER_64`、LSI ≈ 40 kHz → 超时 **≈6.4 s**
     （上面 2.0 s / `RLR = 1250` 是**当时**踩坑的现场值，保留为历史证据；现"开狗 → 首次喂狗"约 1.2 s，
     余量约 5×，规则本身不变。修法与依据见 `docs/lessons.md#iwdg-reload-12bit`）
14. **烧录/调试不必依赖 Keil —— ST 官方工具链已在 VS Code 扩展里解压就绪**
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
     (b) `break <符号名>` 可能命中启动文件里的**弱定义**——`HardFault_Handler` 会解析到
     `0x08000242` 而非向量表里的 `0x0800551C`。**定位异常请用绝对地址**：`break *0x0800551c`
   - `arm-none-eabi-gcc / objdump / nm / addr2line` 14.3.1 位于 `gnu-tools-for-stm32\14.3.1+st.2\bin\`

### 上板调试与复位取证（实测踩坑，逐条都真实发生过）

**★ 最重要的一条：先怀疑自己的测量工具，再怀疑被测系统。★**
本次会话有**多个错误结论**源于观测手段本身在扰动目标。凡"读数自相矛盾"或
"与肉眼观察不符"，先按下面第 10、11、12 条自查。

1. **半主机（semihosting）必须显式关闭。** 不关时库初始化 `__rt_lib_init → _initio →
   _sys_open` 会执行 `bkpt 0x00AB`，无调试器即 HardFault，**在 `main()` 之前就死**，
   表现为"上电毫无反应、串口一个字节都没有"。
   由 `Core/Src/retarget.c` 的 `__use_no_semihosting` + 完整 `_sys_*` + 自写 `fputc` 解决。
   验证方法：镜像里不应再出现 `bkpt`。
2. **`IWDG_RLR` 只有 12 位（`RL[11:0]`，0~4095，RM0008 §18.4.3）。**
   写 `5000` 只会保留低 12 位 `0x388 = 904` → 实际超时 `904×64/40000 = 1.45 s`
   而非以为的 8.0 s。**重装值永远不要超过 4095。**
3. **STM32F1 的 IWDG 一旦启动，系统复位不会清除它**（只有上电复位会）。
   所以配置错了会变成**永久复位循环**，每一轮都沿用同一错配置。
   改狗参数后必须重新上电验证，光按复位键没用。
4. **写 `IWDG_PR`/`IWDG_RLR` 之后必须轮询 `IWDG_SR` 的 PVU/RVU 清零**，
   否则写入被静默忽略。`HAL_IWDG_Init()` 内部就做了这件事，
   **不要为了"照抄参考工程"改成裸寄存器直写** —— 会丢掉那段 SR 轮询。
   参考工程的 `RLR=5000` 同样被截成 904，它没暴露只是因为 `iwdg_start()` 紧贴
   `while(1)`、其后没有耗时动作。**这是"成功案例里的隐藏 bug"，不能照抄。**
5. **`HAL_IWDG_Refresh(&h)` 在 `h.Instance == NULL` 时会写地址 0 → BusFault → HardFault。**
   喂狗请用裸写 `IWDG->KR = 0xAAAAU;`（不依赖句柄）。
6. **`AT+CIPSTART` 是异步的**：先回 `OK`（命令已接受），再回 `CONNECT`（真正建链）。
   **只有 `CONNECT` / `ALREADY CONNECTED` 才算连上**，把 `OK` 当成功会得到
   "板子自认已连接、PC 侧却没有 Established"的假象。
7. **`AT+CIPMODE=1`（透传）必须在 `AT+CIPSTART` 之前设**，顺序是
   `CIPMUX=0 → CIPMODE=1 → CIPSTART → CONNECT → CIPSEND → OK → '>'`。
   先 CIPSTART 再 CIPMODE=1 时，CIPSTART 能拿到 CONNECT、CIPMODE 也回 OK，
   但紧接着的 `AT+CIPSEND` 会直接回 **`ERROR`**，永远进不了透传。
8. **重烧 MCU 不会复位 ESP8266 模块**（模块由整板供电，只有整板断电才复位）。
   上一次若停在透传态，模块会**不解释任何 AT 命令**（把它们当 TCP 数据发出去），
   表现为 `AT+RST` 永远等不到 `ready` → 无限退避、`net` 恒为 0。
   解法：`ESP_RST` 先发**裸 `+++`**（前后各需 ≥20 ms 静默）再发裸 `CRLF` 清残行，
   最后才 `AT+RST`。
9. **绝不能用 `at_send("+++")`** —— 该函数会补 `\r\n`，而 `+++` 转义序列
   **不能带 CR/LF**，带了就失效。必须用裸 `esp_uart_write()`。
10. **SWD 的 `mode=hotplug` 在这台 ST-Link 上每次连接都可能复位目标。**
    `STM32_Programmer_CLI` 启动需 1~2 s，于是**读到的常常是"复位后刚开始启动"的瞬间**：
    `.data` 已加载（如 `g_back_color=0xFFFF`）但 `.bss` 全 0、`uwTick=0`。
    本次会话曾据此得出"面板未识别、宽高为 0"等**完全错误的结论**。
    对策：**一次连接读回整块 RAM**（`-r32 0x20000000 0x2000`），
    让所有字段来自同一瞬间、彼此自洽；上板排查优先用**串口这个非侵入通道**。
11. **PowerShell 5.1 内置别名会吞掉命令**：`r`→`Invoke-History`、
    `rd`/`Rd`→`Remove-Item`、`h`/`H`→`Get-History`。
    凡是"读寄存器函数"起名叫 `r`/`rd`/`h` 的脚本都会读到垃圾数据。
    对策：**读数的脚本一律用 Python 写**（见 `tools/probe.py`），不要用 pwsh 内联。
12. **2 字节对齐的结构体不能用 `-r32` 直接读它的地址**：`lcddev` 位于
    `0x20000126`（2 字节对齐），32 位读会返回错值。
    必须**按 4 字节对齐读、再按字节位置拆字段**。
13. **AC6 把隐式函数声明当错误**（`-Wimplicit-function-declaration`）。
    新增跨模块调用后**必须补对应 `#include`**，否则直接编译失败。
    本次 `Light_IsValid()` / `Dbg_Printf()` / `Protocol_Reset()` 各踩一次。
14. **串口工具的 DTR/RTS 组合决定板子是否被按在复位上。**
    实测可用组合是 **`DTR=False / RTS=False`**；另外三种（含 T/T）收到 0 字节。
    组合不对时表现为"串口只有几个乱码字节"或"开着串口板子不启动、一关就启动"。
15. **`DEBUG_UART=0` 时串口一个字节都不会有**（`Dbg_Init` 是空实现，`USART1` 未初始化）。
    上板诊断前先确认 Keil 工程 `<Define>` 里有 `DEBUG_UART=1`。
16. **复位原因取证**：开机读 `RCC->CSR`（粘滞标志，系统复位不清除），
    读完立刻 `RCC->CSR |= RCC_CSR_RMVF` 清掉，下一轮读到的就精确对应"那一次"。
    判读：`IWDGRSTF`=看门狗、`PINRSTF`=NRST 被外部拉低、`PORRSTF`=掉电、
    `SFTRSTF`=软件复位。本次就是靠它把"屏幕黑↔白闪"定位成 IWDG 复位循环。

### 驱动失败不得致命化（架构性约束）

17. **业务模块一律返回状态码，绝不调用 `Error_Handler()`。**
    `Error_Handler()` 是 `__disable_irq() + for(;;)`；
    而 `MX_IWDG_Init()` 在 `main()` 末尾，`adc_light.c` / `eeprom.c` / `esp8266.c`
    的初始化都**在它之前** → 触发即**永久挂死且连复位都没有**（狗还没开）。
    参考工程把 `Error_Handler` 的调用**限制在 CubeMX 生成的 `Core/` 文件里**，
    `User/App/**` 与 `Drivers/BSP/**` 一次都没有 —— 这就是它守住的那条边界。
18. **采样失败不能与"极值"共用同一个返回值。**
    `Light_ReadRaw` 原来失败时 `return 0`，而 `LIGHT_FROM_ADC(0) = 4095 = 最强光`
    → "传感器故障"被翻译成"光照过强报警"并驱动执行器。
    现在：失败时**保持上次有效值** + `Light_IsValid()` 标志，
    `Control_Loop()` 在无效时**拒绝任何自动决策**（维持现状）。
19. **断链/重连时必须复位协议层的行装配器。**
    断了半帧后重连，新字节会被追加到旧半帧之后 → 第一条指令必然解析失败且
    **静默丢弃**（上位机只看到 ack 超时）。故有 `Protocol_Reset()`，
    由 `on_link_lost()` 调用（参考工程对应的是 `mqtt_rx_reset()`）。
20. **cJSON 是 malloc 型解析器，堆预算必须留足。**
    每字符串/每数字/每节点都分配，一条典型指令约 300~390 B。
    原来 `Heap_Size = 0x200`（512 B）只够 60~77%，再加几个"未知键"（设计上要忽略）
    就越界 → `malloc` 失败 → `cJSON_Parse` 返回 NULL → **整条指令静默丢弃**。
    现已提到 `0x1000`（4 KB，约 10 倍余量）。
21. **`IT` 侧的 relay 与 buzz 已解耦**，报警声走 `ALARM_BEEP_ON_MS` /
    `ALARM_BEEP_OFF_MS`（各 10 s）占空比。**不要退回 `g_buzz = g_relay`** ——
    那会让遮阳动作期间蜂鸣器连续长鸣数小时。
22. **配置宏的单一事实源**：`LIGHT_ADC_MAX` 在 `main.h`（`adc_light.h` 只保留
    `LIGHT_FROM_ADC`）；`PROTOCOL_FRAME_MAX` 在 `protocol.h`（`protocol.c` 引用它）。
    曾出现同一常量三处副本（`200`/`200`/`200`）与四处硬编码 `4095`，改一处必漏改。
23. **`#if DEBUG_UART` 不要写成 `#ifdef DEBUG_UART`** —— `main.h` 里它是
    **无条件定义**的（缺省 0），`#ifdef` 恒为真，语义就反了。

### 工程卫生

24. **地雷文件已移出 `firmware/` 树到 `_attic/`**：`Drivers/_bak_cmsis`、
    `Drivers/_bak_hal`（含 63 个重复 `.c` 与 40+ 组重名 include guard）、
    `Drivers/SYSTEM/usart/usart.c`（重复的 `fputc`/`_sys_*`/`HAL_UART_RxCpltCallback`）、
    `Drivers/BSP/{ADC,LED,LSENS}`（`adc.h` 与 `adc3.h` 共用 `__ADC_H` guard 且
    `ADC_ADCX` 一处是 ADC1 一处是 ADC3；`led.h` 的 PE5 与 `control.c` 的执行器同脚）、
    `Middlewares/cJSON/cjson_test.c`（自带 `main()`）。
    **⚠️ 这些文件一旦被"顺手加进 Keil 工程"就是链接失败或语义冲突。**
    **`usart.h` 必须留在 `Drivers/SYSTEM/usart/`** —— `lcd.c:36` 依赖它（移走会编译失败，
    已实测）。搬移任何文件后**必须重建验证**。
25. **`Drivers/BSP/LCD/lcd_ex.c` 绝不能加入工程**：`lcd.c` 已 `#include` 它，
    再加为独立编译单元会导致 7 个 `lcd_ex_*_reginit` 重复定义。
26. **`lcd.c` / `lcd.h` / `lcd_ex.c` / `lcdfont.h` 与参考工程逐字节相同**，
    且是 **GBK 编码**（`read` 工具会报 `invalid UTF-8 text`，需按 GBK 解码读）。
    **所有 LCD 相关修改都应留在 `Core/Src/lcd_show.c`，不要动 BSP。**
27. **`_template_atk_f103.uvoptx` 不要删** —— `tools/make_keil_project.py` 需要它
    才能生成带 ST-Link 配置的 `greenhouse.uvoptx`；缺失时新克隆会退回 ULINK2，
    报 `Internal DLL Error` / `Flash Download failed`。

## 常用命令

### 推理引擎

- `D:\llama.cpp\模型启动\启动.cmd`：启动 llama-server（端口 **8092**，日志落 `D:\llama.cpp\logs\server.log`）
- `D:\llama.cpp\模型启动\状态.cmd`：查看服务状态与 `/v1/models`
- `D:\llama.cpp\模型启动\停止.cmd`：停止服务
- 验证连通：`curl http://127.0.0.1:8092/health`（期望 `{"status":"ok"}`）
- 服务以 **router 模式**运行（`--models-dir --models-max 1`）：模型按需加载、同一时刻仅驻留一个。
  - 请求体的 `model` 字段取值必须来自 `GET /v1/models`
  - **首次请求 / 切换模型要付权重加载时间**，演示前必须先预热一次；若不需要切模型，直接用固定单模型启动（`-m glm-4.7-flash.gguf`）

### Qt 上位机

- 前置条件：Qt 6.12.0 MinGW 套件需包含 **Qt TextToSpeech** 模块（本机当前未安装，需用 MaintenanceTool 补装；Qt Charts / Network / Sql / Multimedia 已装）
- 构建：`cmake -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=E:/Qt/6.12.0/mingw_64` 然后 `cmake --build build`
- 或用 Qt Creator 打开 `qtapp/CMakeLists.txt` 直接构建运行
- 说明：`CMakeLists.txt` 已就绪；本机 Qt 6.12.0 MinGW 套件**尚缺 Qt TextToSpeech 模块**（见已知陷阱 #6），构建会失败在该模块，需用 MaintenanceTool 补装后再编
- 验证：无 Qt 工具链时可用 `py tools/simulate_node.py --selftest` 离线校验协议逻辑

### STM32 固件

- Keil MDK：打开工程文件直接编译烧录
- 或 CLion + arm-none-eabi-gcc：CMake 构建，ST-Link 烧录
- 说明：固件工程结构见 Tech_Design.md 目录约定，`.ioc` 文件为 CubeMX 初始化来源
- **集成手册（必读）**：`firmware/CUBEMX_SETUP.md` —— 业务代码已自带全部外设初始化，CubeMX **只生成时钟树 + HAL 脚手架 + 启动文件 + 空 MSP**，严禁在 CubeMX 再配 ADC3/I2C1/USART2/GPIO/IWDG 引脚（否则双初始化、抢引脚、破坏 FSMC）
- **编译前自检**：`py firmware/prebuild_check.py` —— 检查 CubeMX 工程是否齐全、有无重复 `USART2_IRQHandler`、cJSON/LCD/wifi_config 是否到位、LCD 默认开显示未关等阻塞项

### 辅助脚本

- **一键构建与自检（推荐入口）**：`powershell -ExecutionPolicy Bypass -File tools\build.ps1`
  - `-Target all|firmware|qt|test`；自动探测 Keil/Qt、自动处理中文路径（见已知陷阱 8）
  - 固件：`UV4.exe -b firmware\Projects\MDK-ARM\greenhouse.uvprojx -j0 -o build.log`
  - 上位机：`cmake -S <ascii>\qtapp -B <ascii>\qtapp\build -G Ninja -DCMAKE_PREFIX_PATH=E:/Qt/6.12.0/mingw_64`
- `py tools/make_keil_project.py`：由官方模板生成 Keil 工程（**增删源文件后必跑**）；`--check` 只校验引用
- **`py tools/consistency_check.py`：跨层一致性守护** —— 管"同一事实在多处出现是否一致"：动作词汇（契约/固件/Qt 三处）、TH/HYS 默认值、端口与监听地址、执行器两阶段配置、PF8/ADC3 引脚与 `light=4095-adc` 方向、**IWDG 溢出周期与重装值（由 `main.c` 现场解析，不硬编码）**、文档集版本。**改任一处后必跑**，漂移即退出码 1
- `py tools/bump_version.py 1.1 "说明"`：4 份文档版本号 + 修订记录一次同步（幂等；改完记得重跑 `md2docx.py`）
- `py tools/simulate_node.py --id 2`：模拟节点 2，连 Qt 演示多点位（默认 127.0.0.1:9092）
- `py tools/simulate_node.py --selftest`：离线冒烟，校验协议状态机，**不连网、可在 CI 跑**
- **`py tools/hil_probe.py`：硬件在环（HIL）验证探针** —— 代替 Qt 当 TCP 服务端，**直接观察实物板发出的每一帧**并主动下发指令，自动核对 9 项（上报周期、`light==4095-adc`、不变式、`get_state`/`alarm_test` 回执、非法阈值回 `err`、合法 `set_th` 生效、**手动优先不回弹**、`auto` 切回自动）。端口须与固件 `wifi_config.h` 一致，**不可与 Qt 上位机同时运行**（抢同一端口）
- **协议单一事实源 + 契约测试（改动任一侧必跑）**：
  - `tools/protocol_schema.py`：动作词汇 / 上行 type / 字段校验的机器可读唯一来源（固件 `Control_OnCommand` 与 Qt `CommandEngine::validate` 都必须与它一致）
  - `py tools/contract_test.py`：11 个契约测试，`get_state` 合法 / `query` 非法、`set_th` 范围与"必须大于回差"不变式、示例帧往返；`py` 直接跑，无需 pytest
- **本机 `python` 命令不可用**（是 Microsoft Store 的占位 stub，执行无输出），一律用 `py`（已验证 Python 3.14.7）
- `py tools/md2docx.py`：由 `项目规划书.md` 生成 `项目规划书.docx`（改文档后必须重跑）

## 目录约定

```text
greenhouse/
├─ AGENTS.md              # 本文件
├─ PRD.md                 # 需求
├─ Tech_Design.md         # 技术方案（引脚唯一事实来源）
├─ 项目规划书.md / .docx   # 对外提交稿（docx 由 md 生成）
├─ 项目内容.txt            # 任务书原文（只读，不要改）
├─ firmware/              # STM32 固件：CubeMX 生成 Core/Drivers/Middlewares，业务代码放 Core/Src 与 Core/Inc
├─ qtapp/                 # Qt 上位机：源码 src/，资源 resources/，构建产物 build/ 不入仓
├─ tests/                 # 主机原生测试（tests/host）：control.c / protocol.c 的行为测试，不覆盖 main.c
├─ docs/                  # lessons.md：踩坑全记录（本文件「已知陷阱」的详版）
├─ tools/                 # 辅助脚本（节点模拟器、协议调试工具、md2docx.py）
└─ README.md
```

> 文档现阶段置于仓库根目录（上表为实际布局）。若后续迁入 `docs/`，必须同步更新本文件、Tech_Design.md 目录树与所有交叉引用。

- 固件模块按职责分文件：`adc_light` / `control` / `esp8266` / `protocol` / `eeprom` / `lcd_show`
- Qt 模块按职责分类：`NodeServer` / `NodeModel` / `ChartWidget` / `ControlPanel` / `TtsService` / `AsrService` / `LlmClient` / `CommandEngine`

## 编码规范

### 固件（C）

- 遵循 HAL 库命名：函数 `HAL_XXX_XXX`，用户模块 `Module_Function`
- 头文件保护用 `#ifndef/#define/#endif`，或 `#pragma once`
- 中断服务函数只置标志位，业务逻辑放主循环
- GPIO 操作用 `HAL_GPIO_WritePin`，不直接操作寄存器（除非有性能特殊需求并注释原因）
- 阻塞延时必须可喂狗：主循环与 AT 等待路径中禁止出现长 `HAL_Delay`（见"安全红线"的 IWDG 条）
- 注释用中文，解释「为什么」而不是复述代码在做什么

### 上位机（C++ / Qt）

- 遵循 Qt 官方编码规范：类名 PascalCase，成员 `m_` 前缀，信号 `signalXXX`，槽 `onXXX`
- 禁止在主线程做阻塞操作（HTTP 请求、文件读写大文件），必须放工作线程或用异步 API
- 网络数据用 Qt 异步信号槽，不手写 `while` 循环等待
- UI 更新只在主线程，工作线程通过信号回主线程
- 一个文件一个主要类，超过 500 行考虑拆分
- 提交前通过编译，不留 `qDebug()` 调试输出与注释掉的死代码

### 通用

- 注释用中文，解释「为什么」而不是复述代码在做什么
- 提交前通过编译与基本测试

## 协议约定

- 传输：TCP + 换行符分隔 JSON（`\n` 结尾），UTF-8 编码
- 所有帧必带 `node` 字段（节点 ID，int，从 1 起）
- 上行 type：`report`（周期上报）/ `ack`（执行回执）/ `evt`（主动事件：`alarm` / `recover`）
- 下行 action：`relay_on` / `relay_off` / `auto` / `set_th` / `get_state` / `alarm_test`
  - `relay_on` / `relay_off`：执行并**切换到手动模式**（手动优先）
  - `auto`：切回自动模式，并按当前 `light` 立即重算输出
  - `set_th`：参数 `value`（0–4095，`light` 单位）且必须 `> hys`，否则返回 `err`
  - `alarm_test`：蜂鸣器脉冲 1 s，**不改模式、不动执行器**，到期恢复联动（PRD 的"报警测试"按钮）
  - 查询状态用 `get_state`，**不存在 `query` 这个动作名**
- 字段命名统一小写（`adc` / `light` / `th` / `hys` / `relay` / `buzz` / `mode`），不用驼峰
- `adc` 为原始 12 位码值（越小越亮），`light = 4095 - adc` 为光强指标；**阈值判断只用 `light`**
- `mode`：0 = 自动，1 = 手动
- 字段含义以 Tech_Design.md 通信协议章节为准，新增字段需同步更新该章节
- JSON 解析固件用 cJSON、Qt 用 QJsonDocument，均忽略未知键（向前兼容）
- 固件侧收包必须做**行缓冲**：`AT+CIPMODE=1` 透传模式下收到的数据**不带 `+IPD` 前缀**，需环形缓冲 + 以 `\n` 断帧，并限制单帧与缓冲上限

## 存储约定

- 阈值存板载 AT24C02（I2C1，地址 0x50），布局 **8 字节**：

  | 偏移 | 长度 | 内容 |
  | --- | --- | --- |
  | 0x00 | 2 | 魔数 `0x47 0x48`（`'G' 'H'`） |
  | 0x02 | 2 | `th`（大端） |
  | 0x04 | 2 | `hys`（大端） |
  | 0x06 | 2 | 校验和 = 前 6 字节之和取低 16 位 |

- 魔数不符或校验失败时用默认值（`th = 2000`，`hys = 150`），**不阻塞启动**
- 加载时必须校验不变式 `0 <= hys < th <= 4095`；不满足则回退默认值
  （否则 `th - hys` 会下溢为负数，比较条件永不成立，继电器将锁死无法释放）
- Qt 配置用 QSettings（组织名 `Greenhouse`，应用名 `LightControl`）
- 历史数据落 CSV 或 SQLite，文件放应用数据目录，不入仓
- 新增 EEPROM 字段必须给默认值或允许校验失败回退，避免破坏存量设备

## 安全红线

- **llama-server 绑 `127.0.0.1`**（只服务本机 Qt）。**Qt NodeServer 必须监听局域网网卡**：默认 `tcp/bindAddress = 0.0.0.0`（或 PC 的 LAN IP）—— 节点是局域网上的**独立设备**，Qt 只绑回环会导致永远连不上；用防火墙把 9092 入站限制在内网
- **端口不可混用**：llama-server = 8092，Qt NodeServer = 9092
- `llm/token` 走配置项；llama-server 实际启动命令未启用 `--api-key`，为空时**不要发送** `Authorization` 头，也不要在源码里硬编码 Token
- `set_th` 参数必须校验：范围 0–4095 **且 `value > hys`**，非法值拒绝并提示，不静默截断
- 大模型返回的 JSON 必须经 `CommandEngine` 校验后再下发，不直接透传给 STM32
- ESP8266 的 WiFi SSID/密码不入仓（用不提交的 `wifi_config.h` + 提交 `wifi_config.h.example`，或 AT 配网）
- 执行器有效电平必须用具名宏 `ACTUATOR_ACTIVE_LEVEL` 表达，不散落魔数；裸继电器必须用三极管/光耦 + 续流二极管，**成品模块已内置驱动元件，无需外接**
- **启用 IWDG（溢出周期约 2 s）**；ESP8266 AT 等待改为带喂狗的有限轮询等待，禁止无喂狗的阻塞延时
- 硬件引脚分配以 **Tech_Design.md 附录 A** 为准；改引脚必须同步附录 A 与项目规划书

## 测试要求

- ADC 采集：手电/遮挡验证**方向正确**（照射 → `light` 上升、`adc` 下降；遮挡 → 相反），滤波后抖动幅度可接受
- 控制逻辑：回差状态机边界值测试（`th`、`th - hys`、死区内）三点各一条用例
- 手动优先：报警中下发 `relay_off` 后输出**不得被自动逻辑回弹**；下发 `auto` 后按当前 `light` 重算
- 上电初始状态：执行器断开、蜂鸣器不响（板载蜂鸣器高电平响，初始化遗漏会一直叫）
- 执行器切换：替换 `ACTUATOR_*` 宏后，`T02/T03/T05` 必须复测通过，且状态机 / 协议 / Qt 侧代码零改动
- 运行指示：LED0 每 5 s 翻转一次，可作"主循环未卡死"的旁证
- **断网本地闭环**：拔掉 ESP8266 或断开 WiFi 后，采集 / 显示 / 报警必须照常工作
- 协议解析：合法 JSON、非法 JSON、缺字段、多余字段、超长无 `\n` 数据均不崩溃
- 阈值校验：`-1` / `4096` / `<= hys` 均被拒绝且不改变现有阈值
- 通信链路：ESP8266 断线重连、Qt 收发、节点离线检测（socket 断开 + 3 s 无上报两种路径）
- 大模型链路：llama-server 启动/未启动两种状态、JSON 合法/非法、超时处理
- 修 bug 时先补一个能复现的测试或调试步骤，再改代码
- 涉及硬件的部分用虚拟节点模拟器（`tools/simulate_node.py`）做无硬件联调

## 提交与协作

> **状态：已生效。** 本仓库已于本次审计中 `git init`（分支 `main`），
> 以下规范从"纸面描述"变为**可执行**。此前的"修订记录"是手工模拟的 changelog，
> 现在请以 `git log` 为准。

- 分支：`main`（保护分支）、`feat/<主题>`、`fix/<主题>`
- 提交信息：Conventional Commits（feat / fix / docs / refactor / test / chore）
- 一个提交只做一件事，不混入无关的格式化改动
- PR 描述写清：改了什么、为什么、怎么验证、影响哪些文档
- 固件与 Qt 代码分开提交，不混在一个 commit

### 基线提交（不要 rebase / 不要 force push 掉它们）

| 提交 | 内容 |
|---|---|
| `9defdeb` | 建立版本控制基线：327 文件全量入仓（**含当时尚未清理的 `_attic/`**） |
| `06aa657` | 删除 6.8 MB 历史遗留副本与参考工程残留（155 文件删除） |

基线提交里刻意保留了清理前的 `_attic/`，**目的就是让那次删除可完全恢复**：

```bash
git checkout 9defdeb -- _attic/          # 取回全部历史遗留副本
git checkout 9defdeb -- _attic/BSP_LED   # 只取回某一个
```

### ★ 交付 / 交作业必须用 `git archive`，不要右键压缩目录 ★

`.gitignore` 只保护 git 仓库，**保护不了"把目录打 zip 发群里"**。
`firmware/Core/Inc/wifi_config.h` 里是真实的 WiFi SSID/密码，
一旦整目录打包就会随包带走 —— 这是 `.gitignore` 无能为力的场景。

正确做法（密钥从原理上不可能入包，因为该文件从未进入任何提交）：

```bash
python tools/package_submission.py               # 生成 submission_src.zip
python tools/package_submission.py --docs-only   # 只打包文档
```

该脚本在打包后**自己校验一遍**：① 列包内文件、确认无 `wifi_config.h` /
`.uvoptx` / `Output/` / `qtapp/build/` 等禁止项；② 从本地 `wifi_config.h`
取出真实 SSID 与密码，在包内**全文搜索**确认零命中。校验失败会自动删包。

### 提交前自检

```bash
git status --porcelain                    # 应为空
git check-ignore -v firmware/Core/Inc/wifi_config.h   # 必须命中 .gitignore
git log --all --pretty=format: --name-only | grep wifi_config   # 应只有 .example
```

## 完成定义（DoD）

- 功能可演示，且通过编译与相关测试
- 涉及需求 / 协议 / 引脚 / 阈值的变更，已同步 `PRD.md`、`Tech_Design.md`、`项目规划书.md` 三份文档，并重跑 `tools/md2docx.py`
- 新增协议字段在 Tech_Design.md 协议章节可见，错误分支有明确中文提示
- 硬件相关改动在真实开发板或虚拟节点上验证过

## 禁止事项

- 不要提交 WiFi 密码、API Token 或模型文件（`.gguf`）
- 不要把业务逻辑写进 UI 信号槽，也不要在固件中断里做耗时操作
- 不要在主线程调用 llama.cpp API（会冻结界面），必须放工作线程
- 不要绕过 `CommandEngine` 直接把大模型输出发给 STM32（缺少校验）
- **不要把执行器或任何外设配到 FSMC 占用脚（PD0/PD1/PD4/PD5/PD8~PD10/PD14/PD15、PE7~PE15、PG0/PG12、PB0）**
- **不要把执行器的端口/引脚/有效电平写死在业务逻辑里** —— 必须走 `ACTUATOR_*` 宏，否则阶段二换真实继电器时要改一堆代码
- **不要直接复用 `lsens_get_val()` 的 0–100 返回值**充当协议里的 `adc`
- 不要手工修改生成的 `项目规划书.docx`；改 `项目规划书.md` 后重新生成
- 不要对 `main` 强推（force push）或改写已推送历史
- 不要擅自引入重量级依赖（新的 GUI 框架、网络库、JSON 库），先说明理由并等确认
- 不要删除或改写他人未合并的分支

## 修订记录

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| **V1.1** | 2026-10-12 | 固件与上位机实现**真实构建**（Keil ARMCLANG 0 错误 0 警告；Qt 6.12+MinGW 构建通过并运行验证监听 0.0.0.0:9092）；补入 HAL/CMSIS/BSP 与 Keil 工程由脚本生成；记录实测 Flash 69.0 KB(13.5%)/RAM 3.1 KB(4.8%)；新增 tools/build.ps1 一键构建（含中文路径规避）与 bump_version.py；修复虚拟节点把 recv 超时误判为断链导致的重连风暴 |
| **V1.0** | 2026-10-12 | **首版基线**（与文档集 V1.0 对齐）。含"已知陷阱"7 条（PF8/ADC3、光照方向、PD11 与接口预留、端口错开、PB8 高电平响、TTS 模块与语音名、LED 分工）；协议含 `auto` / `light` / `hys` / `mode`；EEPROM 8 字节含魔数；IWDG 与断网本地闭环要求；`项目规划书.docx` 由 md 生成 |
