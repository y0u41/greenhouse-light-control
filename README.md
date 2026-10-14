# 🌱 智能温室感光控制系统

![License](https://img.shields.io/badge/license-MIT-green) ![Platform](https://img.shields.io/badge/platform-STM32F103-blue) ![Qt](https://img.shields.io/badge/Qt-6.12-blue) ![Build](https://img.shields.io/badge/build-0%20Error-brightgreen) ![AI](https://img.shields.io/badge/LLM-llama.cpp-orange)

> 基于 STM32 + Qt + 本地大模型的智能温室，光照自动补光/遮阳，支持自然语言控制 🌿

## 这是什么？

一个**软硬结合**的智能温室系统：STM32 固件负责光照闭环控制，Qt 上位机显示实时曲线，还能通过 **llama.cpp 本地大模型**用自然语言控制设备（无需联网）。

**适合场景**：
- 🌱 智能农业 / 温室自动化
- 📚 学习 STM32 + Qt 上位机开发
- 🤖 本地大模型嵌入式应用实战

## ✨ 功能特性

### 硬件控制
- ☀️ **光照闭环**：ADC 采集 → 阈值判断 → 自动补光/遮阳
- 🛡️ **本地自治**：断网时固件端闭环照常工作

### 数据上报
- 📡 **WiFi 传输**：ESP8266 AT 透传，1s 周期上报
- 📶 **断线重连**：自动检测并恢复连接

### 上位机
- 📊 **Qt 6 界面**：实时曲线、控制面板、节点状态
- 🤖 **自然语言控制**：llama.cpp 本地大模型，无需联网

## 🛠️ 技术栈

| 层 | 技术 |
|---|---|
| 固件 | STM32F103 + Keil MDK + HAL |
| 通信 | ESP8266 AT + TCP:9092 |
| 上位机 | Qt 6.12 + CMake + MinGW |
| AI | llama.cpp（本地 127.0.0.1:8092） |

## 📁 目录结构

```
greenhouse-light-control/
├── firmware/               # STM32 固件
│   ├── User/App/           # 应用层
│   └── Projects/MDK-ARM/   # Keil 工程
├── host/                   # Qt 上位机
├── tools/                  # 构建与测试脚本
│   ├── build.ps1           # 一键构建
│   └── contract_test.py    # 协议测试
└── docs/                   # 项目文档
```

## 🚀 快速开始

```powershell
# 一键构建（固件 + 上位机 + 自检）
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 只构建上位机
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target qt

# 只跑测试
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target test
```

**构建状态**：
- ✅ 固件：0 Error / 0 Warning
- ✅ 协议测试：11/11 通过
- ✅ Qt 上位机：构建通过

## 📚 文档

| 文档 | 内容 |
|------|------|
| [设计报告.md](设计报告.md) | 系统设计与验收记录 |
| [Tech_Design.md](Tech_Design.md) | 技术细节与接口定义 |
| [README_Hardware.md](README_Hardware.md) | 硬件接线说明 |

## 📄 许可证

[MIT License](LICENSE) © y0u41

- ☀️ **光照闭环控制**：ADC 采集 → 阈值 + 回差判断 → LED/继电器自动补光或遮阳
- 📡 **WiFi 数据上报**：ESP8266 AT 透传，1s 周期上报，断网自动重连
- 📊 **Qt 上位机**：实时曲线、控制面板、节点状态监控
- 🤖 **本地大模型**：llama.cpp 自然语言 → JSON 指令，无需联网
- 🛡️ **本地自治**：断网/上位机未启动时，固件端闭环照常工作

## 快速开始

```powershell
# 一键构建（固件 + 上位机 + 自检）
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 只构建上位机
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target qt

# 只跑自检
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target test
```

## 系统架构

```text
光敏(PF8/ADC3_IN6) → STM32 本地闭环(阈值+回差+手动优先) → 执行器/蜂鸣器
                         │ 1 s 上报（同时充当链路心跳）
                         ▼
ESP8266(AT 透传) ──TCP:9092──▶ Qt NodeServer ──▶ 曲线/控制面板
                                        │
                                        └─▶ llama.cpp(127.0.0.1:8092) 自然语言→JSON 指令
```

- **本地自治**：断网 / 上位机未启动 / 大模型不可用时，STM32 采集、显示、报警闭环照常工作。
- **执行器两阶段**：阶段一板载 LED1(PE5) 作等效负载；阶段二借用继电器(PD11) 只换 `control.c` 里 4 行宏，业务代码零改动（Tech_Design 附录 A.5/A.6）。
- **端口纪律**：llama-server = **8092**（绑 `127.0.0.1`）；Qt NodeServer = **9092**（绑局域网网卡，节点是独立设备，**不能绑回环**）。

## 构建状态（已实测）

| 部分 | 工具链 | 结果 |
| --- | --- | --- |
| STM32 固件 | Keil MDK 5.36 / ARMCLANG V6.24 | **0 Error(s), 0 Warning(s)** |
| Qt 上位机 | Qt 6.12.0 + MinGW gcc 13.1.0 + CMake 3.30.5 + Ninja | **构建通过，可运行** |
| 协议契约 | `py tools/contract_test.py` | **11/11 通过** |
| 虚拟节点 | `py tools/simulate_node.py --selftest` | **通过** |
| 上位机联调 | 启动 exe + 虚拟节点接入 | **监听 `0.0.0.0:9092`，长连接 ESTABLISHED** |
| 真机硬件在环 | ST-Link SWD + COM3 日志 | **T01 换算/量程、T02 执行器+蜂鸣器+GPIO 极性、T03 释放方向、T08 上电初始状态已实测**（详见 [设计报告.md §8.4](设计报告.md)） |

固件资源占用（实测，来自 `firmware/Output/greenhouse.map`）：

| 项 | 值 | 占比 |
| --- | --- | --- |
| Flash | Code 64012 + RO 14212 + RW 24 = **78248 B（76.4 KB）** | 512 KB 的 **14.9%** |
| RAM | RW 24 + ZI 7672 = **7696 B（7.5 KB）** | 64 KB 的 **11.7%** |

## 一键构建（推荐）

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1              # 固件 + 上位机 + 全部自检
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target qt   # 只构建上位机
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target test # 只跑自检
```

脚本自动处理两件本机特有的事：探测 Keil（`D:\MDK5.36`）与 Qt（`E:\Qt`）并注入 PATH；源码路径含中文时自动建立 ASCII 目录联接后构建（原因见下）。

> **⚠️ 源码路径必须是纯 ASCII，否则 Qt 的 moc 会失败**
> 本机源码路径 `D:\正点原子\个人项目实战\智能温室感光控制-华清远见` 含中文，而 Qt 的 AutoMoc
> **无法把生成文件写进非 ASCII 路径**，会报
> `moc: Cannot create ...\moc_xxx.cpp. Error: No such file or directory`。
> 注意**不能**用 8.3 短路径绕过——本机短路径仍是中文（`D:\正点原子\个人项~1\智能温~1`）。
> `build.ps1` 的做法是建立一个纯 ASCII 联接 `D:\glc -> 项目根`，经由它构建；源码位置不变。
> 手工构建时请照做，或把工程放到纯 ASCII 路径下。

## 目录结构

```text
firmware/                     STM32 固件（HAL）
├─ Core/Inc|Src/              业务模块：main / adc_light / control / protocol / eeprom / esp8266 / lcd_show
│                             + CubeMX 约定件：stm32f1xx_hal_conf.h、stm32f1xx_it.c/h
├─ Drivers/                   HAL + CMSIS + 官方 BSP（从资料盘拷入；BSP 含 LCD）
├─ Middlewares/cJSON/         下行 JSON 解析
├─ Projects/MDK-ARM/          greenhouse.uvprojx（由脚本生成）、_template_atk_f103.uvprojx
├─ Output/                    Keil 产物（不入仓）
├─ CUBEMX_SETUP.md            CubeMX/外设配置清单
└─ prebuild_check.py          编译前静态自检（缺文件 / 双初始化 / 引脚主权）
qtapp/                        Qt 6 上位机
├─ CMakeLists.txt
└─ src/                       MainWindow（纯 UI 装配 / 信号转发）/ NodeServer / NodeModel /
                              CommandEngine / CommandAckGuard /
                              CsvLogger（历史数据落盘）/ LlmClient / LlmController（LLM 线程 +
                              /health 探测 + 可用性）/ ChartWidget / ControlPanel /
                              TtsService / AsrService / VoiceController（TTS 播报 + ASR 识别 +
                              播报开关）/ Config
                              protocol_actions.h（协议词汇 C++ 侧单一事实源）
tools/
├─ protocol_schema.py         协议契约单一事实源（动作词汇 + 字段校验）
├─ contract_test.py           协议契约测试（动作词汇 / set_th 边界 / 示例帧）
├─ consistency_check.py       跨层一致性守护（固件 ↔ Qt ↔ 契约 ↔ 文档）
├─ simulate_node.py           虚拟节点（多点位演示 / 协议自测）
├─ hil_probe.py               硬件在环验证探针（代替 Qt 当 TCP 服务端，核对协议不变式）
├─ probe.py                   上板 SWD 探针（板子无响应时读 uwTick/寄存器/故障栈帧取证，
│                             不依赖网络；与 hil_probe.py 分属芯片级 / 协议级，互补不重复）
├─ package_submission.py      交付打包（git archive + 密钥结构性排除，包后自校验）
├─ make_keil_project.py       由模板生成 greenhouse.uvprojx（含引用校验）
├─ build.ps1                  一键构建与自检（含中文路径规避）
├─ bump_version.py            文档集版本号 + 修订记录一次同步
└─ md2docx.py                 项目规划书 md → docx
tests/
└─ host/                      主机端行为测试（原生 gcc 编译 control.c / protocol.c / eeprom.c 原文）
```

## 快速开始（分步）

### 1. 协议契约（无需硬件）

```bash
py tools/contract_test.py              # 11 个契约测试，必须全绿
py tools/consistency_check.py          # 38 项跨层一致性（固件/Qt/契约/文档 四处对齐；数目以脚本输出为准）
py tools/simulate_node.py --selftest   # 虚拟节点状态机自测（含手动优先、断帧残留）
py tests\host\run_host_tests.py        # 主机端行为测试（control 129 + protocol 68 + eeprom 41 用例）
py firmware/prebuild_check.py          # 固件编译前自检
```

> `run_host_tests.py` 用 `tests/host/shim/stm32f1xx_hal.h` 垫片在 x86 上编译
> `firmware/Core/Src/control.c` / `protocol.c` 的**原文**（不复制、不改动），
> 改控制逻辑后**不必烧板**即可秒级回归；gcc 自动探测（首选 `E:\Qt\Tools\mingw1310_64\bin\gcc.exe`，
> 也可 `--gcc <path>` 指定）。已接入 `tools/build.ps1 -Target test`。

### 2. STM32 固件

1. 复制 `firmware/Core/Inc/wifi_config.h.example` 为 `wifi_config.h`，填 WiFi 与 **PC 的局域网 IP**（不入仓；缺失时 main.c 用占位值，能编译但连不上网）。
2. `py tools/make_keil_project.py` 生成/刷新 Keil 工程（改过源文件列表后必跑）。
3. Keil 打开 `firmware/Projects/MDK-ARM/greenhouse.uvprojx` 编译烧录；命令行等价于
   `UV4.exe -b greenhouse.uvprojx -j0 -o build.log`。
4. 详细外设配置与"引脚主权"清单见 `firmware/CUBEMX_SETUP.md`。

> 若用 CubeMX 重新生成工程：**必须删掉 `Core/Src/stm32f1xx_it.c` 里的 `USART3_IRQHandler`**
> （已在 `esp8266.c` 定义），且不要让 CubeMX 重复初始化 PF8 / PB6-PB7 / PB10-PB11 / PB8 / PE5 / PB5。
> `firmware/prebuild_check.py` 会**机器校验**：`USART3_IRQHandler` 是否重复定义、`STM32F103xE`
> 设备宏、以及 `Core/Src/system_stm32f1xx.c` 与启动文件是否存在。
> **引脚主权（PF8/PB6-PB7/PB10-PB11/PB8/PE5/PB5）它只打印清单提醒、不做机器校验**，
> 需人工按 `firmware/CUBEMX_SETUP.md` 的归属表核对。

### 3. Qt 上位机

```powershell
# 经 ASCII 联接构建（路径含中文时必需）
$env:PATH = "E:\Qt\Tools\mingw1310_64\bin;E:\Qt\Tools\CMake_64\bin;E:\Qt\Tools\Ninja;" + $env:PATH
cmake -S D:\glc\qtapp -B D:\glc\qtapp\build -G Ninja -DCMAKE_PREFIX_PATH=E:/Qt/6.12.0/mingw_64 -DCMAKE_BUILD_TYPE=Release
cmake --build D:\glc\qtapp\build
```

前置：Qt 6.12.0 MinGW 套件需含 **Widgets / Network / Charts**。
**TextToSpeech 需用 MaintenanceTool 补装**；未装时 CMake 自动探测并关闭（播报置灰，其余功能不受影响），补装后重新 configure 即自动开启。

### 4. llama.cpp（外部依赖，已存在）

服务管理脚本（含 `重启.cmd`）都在 `D:\llama.cpp\模型启动\`，日志落 `D:\llama.cpp\logs\server.log`：

```bash
D:\llama.cpp\模型启动\启动.cmd        # 启动：8092，router 模式（模型按需加载）
D:\llama.cpp\模型启动\状态.cmd        # 查看服务是否在跑
D:\llama.cpp\模型启动\停止.cmd        # 停止
curl http://127.0.0.1:8092/health    # {"status":"ok"}
```

演示前先预热一次请求（router 模式下首次请求含权重加载时间）。

> 服务状态判据、router 模式与"工具会抢端口"的坑见 `docs/lessons.md#ports`。

## 运行

### 上位机 + 虚拟节点（无硬件即可演示全链路）

```powershell
# 1) 启动 llama-server（可选；不启动则智能交互置灰，监控与按钮控制照常）
D:\llama.cpp\模型启动\启动.cmd

# 2) 启动上位机（需让 Qt 运行库可见，否则报缺少 Qt6Core.dll）
$env:PATH = "E:\Qt\6.12.0\mingw_64\bin;" + $env:PATH
.\qtapp\build\GreenhouseLightControl.exe

# 3) 另开一个终端，接入虚拟节点 2（演示多点位）
py tools\simulate_node.py --id 2 --port 9092
```

自检：`netstat -ano | findstr :9092` 应看到 `0.0.0.0:9092 LISTENING`；节点接入后出现 `ESTABLISHED`（长连接，不应反复重连）。

> 要发给别人演示，用 `windeployqt` 把 Qt 运行库拷到 exe 旁边，否则对方机器缺 DLL：
> `E:\Qt\6.12.0\mingw_64\bin\windeployqt.exe qtapp\build\GreenhouseLightControl.exe`

### 真机节点

1. 填好 `firmware/Core/Inc/wifi_config.h`（WiFi 名称/密码 + **PC 的局域网 IP**，端口固定 9092）
2. Keil 打开 `firmware/Projects/MDK-ARM/greenhouse.uvprojx` 编译；用 ST-Link 烧录 `firmware/Output/greenhouse.hex`
3. **Windows 防火墙放行 9092 入站**，否则节点连不上（Qt 已绑 `0.0.0.0`，缺的是防火墙规则）
4. PC 建议设静态 IP 或 DHCP 保留，避免 IP 变化后节点配置失配
5. 上电后应看到：LCD 显示光强/阈值/执行器/模式；LED0 每 5 s 翻转（心跳）；手电照射 → `light` 上升 → 超阈值时执行器吸合 + 蜂鸣器响；遮挡回落到 `th - hys` 以下才释放

### 硬件在环验证（不依赖 Qt 界面）

板子是 TCP **客户端**，Qt 是服务端；要独立验证固件时，用探针自己当服务端 —— 既能逐帧看到板子
真实发出的 `report` / `ack` / `evt`，又能主动下发指令并核对回执：

```powershell
py tools\hil_probe.py                  # 监听 0.0.0.0:9092，跑完整指令序列并打印 PASS/FAIL
py tools\hil_probe.py --listen-only    # 只听不发，纯观察上报流
py tools\hil_probe.py --port 9092 --duration 24
```

自动核对：上报周期 ≈1 s、每帧 `light == 4095 - adc`、不变式 `0 <= hys < th <= 4095`、
非法 `set_th` 回 `err` 且不改阈值、**手动优先**不被自动逻辑回弹、`auto` 切回自动。

> ⚠ **不可与 Qt 上位机同时运行**（两者都绑 9092）；端口须与固件 `wifi_config.h` 的
> `TCP_SERVER_PORT` 一致。

### 板子无响应时的 SWD 取证（不依赖网络）

`hil_probe.py` 要求固件已跑起来且 WiFi 连上；板子卡死/复位循环/HardFault 时它完全够不着，
此时改用 ST-Link 直读芯片内存的 `probe.py`：

```powershell
py tools\probe.py --check                    # 采样 uwTick，判定是否在复位循环
py tools\probe.py --read g_adc g_th g_light  # 按符号名读全局变量（需 .axf 在 Output/）
py tools\probe.py --core                     # 读 PC/SP/LR/XPSR，看卡在哪
py tools\probe.py --fault                    # 读 CFSR/HFSR + 异常栈帧，定位 HardFault
```

> 两者分工：`probe.py` = 芯片级（SWD，需要 ST-Link，断网可用）；`hil_probe.py` = 协议级
> （TCP，需要节点在线）。互为补集，都要保留。
> `probe.py` 用严格正则只认 `0xAAAAAAAA : WWWWWWWW` 数据行，避免把 "ST-LINK SN" 之类误当读数
> （起因与教训见 `docs/lessons.md`）。

### 历史数据落盘（CSV）

PRD「数据组织 · 数据存储」要求"历史数据：Qt 端 CSV 或 SQLite 落盘，便于绘制长期曲线"。
实现取 **每帧 append CSV**（不做 SQLite）；曲线仍只保留最近 `chart/maxPoints` 点，长期曲线以文件为准。
实现与取舍见 `qtapp/src/csvlogger.h`，本表是**运行视角的唯一说明**（PRD 只声明需求，不重复这些字段）。

| 项 | 值 |
| --- | --- |
| 默认路径 | exe 同级 `data/history.csv`（如 `qtapp\build\data\history.csv`）；父目录不存在时自动创建 |
| 配置项 | QSettings `storage/historyCsv`（组织 Greenhouse / 应用 LightControl）；相对路径按 **exe 目录**解析；置空字符串 = 关闭落盘 |
| 表头 | `timestamp,node,adc,light,th,hys,relay,buzz,mode` |
| 时间戳 | ISO8601 毫秒 + 显式时区偏移，如 `2026-10-12T21:01:51.410+08:00` |
| 行内容 | 与 `report` 帧字段一一对应；`node` 列区分多点位（同一文件含多节点行）；帧里缺的字段写**空列**，不用 `0` 冒充（`0` 是合法取值） |
| 编码/行尾 | UTF-8 无 BOM，CRLF（Windows 下双击可进 Excel/WPS） |
| 何时写 | 收到 `report` 帧即刻 append；写点在 ACK 守卫过滤**之前** —— 守卫只影响界面渲染，不影响留档 |
| flush | 每帧 `write`（进程内无缓冲，强杀/结束任务不丢已写行）+ 每 8 行或每 1 s `flush` 一次，退出时必 flush |
| 失败行为 | 打开/写入失败只提示**一次**（状态栏 + 对话区 + `qWarning`），之后静默跳过，程序照常运行 |
| 入仓 | 数据文件**不入仓**（`.gitignore` 含 `*.csv`）；`csvlogger.h/.cpp` 等代码与本文档入仓 |

> 只记 `report`；`ack`/`evt` 不入盘（`evt` 只带 `adc`/`light`，`ack` 是同一状态的回执，重复留档无新信息）。

## 交付打包（交作业 / 发群）

**★ 不要右键压缩目录 ★** —— `.gitignore` 只保护 git 仓库，保护不了"把目录打成 zip 发出去"；
`firmware/Core/Inc/wifi_config.h` 里的真实 WiFi SSID / 密码会随包带走。

```powershell
py tools\package_submission.py                  # 源码包 submission_src.zip
py tools\package_submission.py --docs-only      # 只打包文档（*.md + docs/），体积更小
py tools\package_submission.py -o D:\提交.zip   # 指定输出路径
```

打包走 `git archive HEAD` —— 只导出**受版本控制**的文件，而 `wifi_config.h` 从未进入任何提交，
所以**从原理上不可能进包**（比"人工核对"可靠）。打包后脚本还会自校验：

1. **路径级**：包内不得出现 `wifi_config.h` / `.env` / `*.uvoptx` / `*.uvguix.*` /
   `firmware/Output/` / `qtapp/build/` / `_attic/` / `__pycache__` / `.log` / `.git/`；
2. **内容级**：从本地 `wifi_config.h` 取出真实 SSID / 密码 / PC IP，在包内**全文搜索**，
   命中即判失败，并**自动删除已生成的包**。

提交前的自检清单（`git status --porcelain` 为空、`wifi_config.h` 被 `.gitignore` 命中）见 `AGENTS.md` §八。

## 里程碑状态

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| M1 | ADC3/PF8 采集、8 次均值、`light=4095-adc`、LCD 显示 | **已上板**：换算/量程/LCD `id=0x7796 480×320` 实测通过；**光照物理方向待人手实验** |
| M2 | 回差状态机、手动优先、EEPROM 掉电记忆、IWDG、LED0 心跳 | **已上板**：执行器吸合/释放 + 蜂鸣器 + GPIO 极性实测通过；`set_th` 观测到回 `err` 并回滚（EEPROM/I2C 状态不稳定，**待独占设备复核**，见 [设计报告.md §8.4](设计报告.md) N-1/N-2） |
| M2b | 借用继电器到位后切换 4 行宏复测 | 待硬件 |
| M3 | ESP8266 联网、TCP 透传、行缓冲、report 上报 | **已上板**：AT 建链成功过（`WIFI CONNECTED`→`GOT IP`→`CIPSTART`，`net=1`，PC 侧收到 report）；**AP `603` 当前不可达**，重连/上报待恢复网络后复测 |
| M4 | Qt NodeServer(9092)、控制面板、曲线、离线判定 | **已构建并运行验证**（监听 0.0.0.0:9092，虚拟节点可接入） |
| M5 | LlmClient 工作线程、System Prompt、CommandEngine 校验 | 代码完成，待与 llama-server 实测 |
| M6 | TTS/ASR、三路指令统一 | 代码完成；**TTS 需补装模块**，ASR 中文精度待实测 |
| M7 | 多节点、模拟器、降级、30 分钟稳定性 | 模拟器可用；**资源占用已实测**；30 分钟稳定性待整机 |
| M8 | 报告与答辩 | 进行中 |

> 状态口径：**已编译通过** = 真实工具链构建成功（固件 0 错误 0 警告；上位机可运行）；
> **待上板** = 需要真实硬件才能验证的项，不要写成"已验证"。

## 项目文档

| 文档 | 说明 |
|------|------|
| [PRD.md](PRD.md) | 产品需求文档 |
| [Tech_Design.md](Tech_Design.md) | 技术方案（引脚/协议/阈值唯一事实源） |
| [设计报告.md](设计报告.md) | 实测数据与验收报告 |
| [AGENTS.md](AGENTS.md) | 协作约定与已知陷阱 |

## 许可证

[MIT License](LICENSE) © y0u41
