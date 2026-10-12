# 智能温室感光控制系统

STM32 本地闭环 + ESP8266 上报 + Qt 上位机 + llama.cpp 本地大模型的温室感光控制。

- 需求以 [PRD.md](PRD.md) 为准
- 技术方案（**引脚 / 电平极性 / 协议 / 阈值的唯一事实来源**）以 [Tech_Design.md](Tech_Design.md) 为准
- 协作约定与已知陷阱见 [AGENTS.md](AGENTS.md)
- 对外提交稿见 [项目规划书.md](项目规划书.md) / [项目规划书.docx](项目规划书.docx)（docx 由 md 生成，勿手改）
- **设计报告（任务书第 4 项）见 [设计报告.md](设计报告.md) / [设计报告.docx](设计报告.docx)** —— 实测数据与【待测】项严格区分，未实测不填推测值

## 系统形态

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

固件资源占用（实测，来自 `firmware/Output/greenhouse.map`）：

| 项 | 值 | 占比 |
| --- | --- | --- |
| Flash | Code 57260 + RO 13360 + RW 32 = **70652 B（69.0 KB）** | 512 KB 的 **13.5%** |
| RAM | RW 32 + ZI 3136 = **3168 B（3.1 KB）** | 64 KB 的 **4.8%** |

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
└─ src/                       MainWindow / NodeServer / NodeModel / CommandEngine / CommandAckGuard /
                              CsvLogger（历史数据落盘）/ LlmClient / ChartWidget / ControlPanel /
                              TtsService / AsrService / Config
                              protocol_actions.h（协议词汇 C++ 侧单一事实源）
tools/
├─ protocol_schema.py         协议契约单一事实源（动作词汇 + 字段校验）
├─ contract_test.py           协议契约测试（动作词汇 / set_th 边界 / 示例帧）
├─ consistency_check.py       跨层一致性守护（固件 ↔ Qt ↔ 契约 ↔ 文档）
├─ simulate_node.py           虚拟节点（多点位演示 / 协议自测）
├─ make_keil_project.py       由模板生成 greenhouse.uvprojx（含引用校验）
├─ build.ps1                  一键构建与自检（含中文路径规避）
├─ bump_version.py            文档集版本号 + 修订记录一次同步
└─ md2docx.py                 项目规划书 md → docx
```

## 快速开始（分步）

### 1. 协议契约（无需硬件）

```bash
py tools/contract_test.py              # 11 个契约测试，必须全绿
py tools/consistency_check.py          # 30 项跨层一致性（固件/Qt/契约/文档 四处对齐）
py tools/simulate_node.py --selftest   # 虚拟节点状态机自测（含手动优先、断帧残留）
py firmware/prebuild_check.py          # 固件编译前自检
```

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

```bash
D:\llama.cpp\模型启动\启动.cmd        # 8092，router 模式（模型按需加载）
curl http://127.0.0.1:8092/health    # {"status":"ok"}
```

演示前先预热一次请求（router 模式下首次请求含权重加载时间）。

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

## 里程碑状态

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| M1 | ADC3/PF8 采集、8 次均值、`light=4095-adc`、LCD 显示 | 代码完成，**已编译通过**，待上板实测 |
| M2 | 回差状态机、手动优先、EEPROM 掉电记忆、IWDG、LED0 心跳 | 代码完成，**已编译通过**，待上板实测 |
| M2b | 借用继电器到位后切换 4 行宏复测 | 待硬件 |
| M3 | ESP8266 联网、TCP 透传、行缓冲、report 上报 | 代码完成，**已编译通过**，待上板实测 |
| M4 | Qt NodeServer(9092)、控制面板、曲线、离线判定 | **已构建并运行验证**（监听 0.0.0.0:9092，虚拟节点可接入） |
| M5 | LlmClient 工作线程、System Prompt、CommandEngine 校验 | 代码完成，待与 llama-server 实测 |
| M6 | TTS/ASR、三路指令统一 | 代码完成；**TTS 需补装模块**，ASR 中文精度待实测 |
| M7 | 多节点、模拟器、降级、30 分钟稳定性 | 模拟器可用；**资源占用已实测**；30 分钟稳定性待整机 |
| M8 | 报告与答辩 | 进行中 |

> 状态口径：**已编译通过** = 真实工具链构建成功（固件 0 错误 0 警告；上位机可运行）；
> **待上板** = 需要真实硬件才能验证的项，不要写成"已验证"。
