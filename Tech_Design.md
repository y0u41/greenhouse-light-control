# 智能温室感光控制系统 Tech Design

> 版本：**v1.1**（2026-10-12）
> 配套文档：[PRD.md](PRD.md)（需求事实来源）、[项目规划书.md](项目规划书.md)（对外提交稿）
> **本文件附录 A 是引脚分配、电平极性、ADC 与阈值语义的唯一事实来源。**其余文档中的同类内容均为镜像，如有冲突以本附录为准。

---

## 技术选型

### 现场设备端（STM32）

| 项 | 选型 |
| --- | --- |
| 主控 | STM32F103ZET6（正点原子精英版 V2，Cortex-M3，72 MHz，512 KB Flash / 64 KB SRAM） |
| 固件框架 | HAL 库（CubeMX 生成初始化，手写业务逻辑） |
| 开发工具 | Keil MDK-ARM 或 CLion + arm-none-eabi-gcc；下载器 ST-Link / J-Link（SWD） |
| 光照采集 | 板载光敏电阻 LS1 → **PF8（ADC3_IN6）**，12 位，0–4095，8 次均值滤波 |
| 光强换算 | `light = 4095 - adc`（见附录 A.2） |
| 报警输出 | 板载有源蜂鸣器（**PB8，高电平响**）+ 执行器（**目标：外接继电器模块 PD11，渠道已确认可借用**；**先验阶段：板载 LED1 / PE5 等效负载**，见附录 A.5） |
| 状态指示 | LED0（PB5，红）作运行心跳，**每 5 s 翻转一次**；LED1（PE5，绿）当前兼作执行器指示 |
| 显示 | 板载 FSMC LCD 接口 + 3.5 寸 TFTLCD（NT35310，480×320，MCU 屏 16 位并口） |
| 无线通信 | ESP8266（AT 固件），USART2（PA2/PA3），TCP 透传 |
| 阈值存储 | 板载 AT24C02 EEPROM（I2C1，PB6/PB7），8 字节布局含魔数与校验和 |
| 看门狗 | IWDG，溢出周期约 2 s |
| JSON | cJSON 库（轻量 C 实现，仅编译所需模块） |

### 上位机（Qt）

| 项 | 选型 |
| --- | --- |
| 框架 | Qt 6.12.0 + MinGW 13.1.0 64-bit（`E:\Qt`） |
| **前置依赖** | **Qt TextToSpeech 模块（本机未安装，需用 Qt MaintenanceTool 补装）** |
| UI | Qt Widgets（比 QML 更适合本项目的表单型监控界面） |
| 网络 | QTcpServer（**9092** 收节点数据）+ QNetworkAccessManager（调推理 API） |
| 曲线 | Qt Charts（QChart）实时光强曲线 |
| 语音合成 | Qt TextToSpeech + Windows 中文语音（默认 `Microsoft Huihui`，运行时枚举回退） |
| 语音识别 | **主用 PowerShell + `System.Speech`（SAPI）听写**，免 C++/WinRT 互操作；备选 WinRT `SpeechRecognizer`（中文精度通常更好，需实测对比） |
| 数据持久化 | QSettings（配置）+ CSV / SQLite（历史数据） |
| 线程 | QThread / QtConcurrent（网络收发与 LLM 调用不阻塞 UI） |

> **语音链路的前提条件**：Qt TextToSpeech 模块必须安装；语音识别需要系统中文语音包与麦克风权限。任一项不可用时按 §Qt 上位机设计·语音链路降级 处理，不得让应用崩溃或卡死。

### 推理引擎

| 项 | 值 |
| --- | --- |
| 运行时 | llama.cpp Server（`D:\llama.cpp`），OpenAI 兼容 API |
| 端口 | `127.0.0.1:8092`，仅回环监听 |
| **运行模式** | **router 模式**：`llama-server.exe --models-dir "D:\llama.cpp\models" --models-max 1 -c 32768 -ctk q8_0 -ctv q8_0 --port 8092 --host 127.0.0.1`，模型**按需加载、同一时刻仅驻留一个** |
| 主用模型 | `glm-4.7-flash.gguf`（MoE，活跃约 2.6 B，12.38 GB） |
| 备用模型 | `qwen3.8-27b-turbo.gguf`（稠密 27 B，11.29 GB，仅非实时场景） |
| 模型标识 | 请求体 `model` 字段**必须取自 `GET /v1/models`** 的返回列表，不要硬编码猜测 |
| 认证 | 当前启动命令**未启用 `--api-key`**；配置项 `llm/token` 为空时不发送 `Authorization` 头 |
| 启动方式 | `D:\llama.cpp\模型启动\启动.cmd`（端口检测 + 日志落盘） |

> **两条硬约束**（都是实测/代码级事实，不是估计）：
> 1. `启动.cmd` 以 `netstat :8092 LISTENING` 判断服务状态 → **Qt 绝不能监听 8092**，否则脚本会误报"服务已在运行"。
> 2. `--models-max 1` + 8 GB 显存 → 切换模型会触发权重换入换出。**首次请求或切模型后的首个请求包含加载时间**，演示前必须先预热一次；若不需切模型，直接改用固定单模型启动（`-m glm-4.7-flash.gguf`）以彻底规避。

### 工程与协作

- 版本控制：Git，固件与上位机 monorepo 分目录
- 代码风格：固件遵循 HAL 库命名；Qt 侧遵循 Qt 官方编码规范
- 文档：Markdown 置于仓库根目录；`项目规划书.docx` 由 `tools/md2docx.py` 从 `项目规划书.md` 生成，**不手工编辑 docx**

---

## 系统架构

### 分层结构

- **采集层（STM32）**：ADC3 读取、滤波、光强换算、阈值 + 回差判断、执行器（继电器）/蜂鸣器控制，**纯本地闭环，不依赖网络**
- **传输层（ESP8266 + TCP）**：AT 指令联网，JSON 帧收发，断线重连
- **展示与控制层（Qt）**：数据显示、曲线、按钮、语音、指令校验与下发
- **智能层（llama.cpp）**：Qt 进程内 HTTP 调用，自然语言 → 结构化 JSON 指令

依赖方向固定为：Qt → llama.cpp API（单向调用）；Qt ↔ STM32（双向 TCP）。

### 请求处理链路

**数据上报（STM32 → Qt）**

```text
ADC3 采样(PF8) → 8 次均值 → light = 4095 - adc → LCD 刷新
              → 组 JSON report → ESP8266 TCP → Qt NodeServer(9092) 解析
              → 更新 NodeModel → 界面 / 曲线刷新
```

**控制下发（Qt → STM32，三路合一）**

```text
按钮点击 / ASR 文本 / 大模型返回 → CommandEngine 组装 cmd
    → 校验（动作合法、参数范围、阈值 > 回差、节点在线）
    → TCP 下发 → STM32 执行（relay_on/relay_off 同时切手动模式）
    → 回 ack → Qt 更新状态 + TTS 播报
```

**大模型解析（Qt 进程内）**

```text
用户输入 → 组 messages（注入当前 light/th/relay/mode）→ POST /v1/chat/completions
        → 解析 choices[0].message.content 为 JSON → 校验 → 进入 CommandEngine
```

### 目录结构

```text
greenhouse/
├─ AGENTS.md                        # 协作约定与已知陷阱
├─ PRD.md                           # 需求事实来源
├─ Tech_Design.md                   # 本文件（引脚唯一事实来源）
├─ 项目规划书.md / 项目规划书.docx    # 对外提交稿（docx 由 tools/md2docx.py 生成）
├─ 项目内容.txt                      # 任务书原文（只读）
├─ firmware/                        # STM32 固件
│  ├─ Core/
│  │  ├─ Inc/
│  │  │  ├─ main.h
│  │  │  ├─ adc_light.h             # ADC3(PF8) 采集、光强换算、滤波
│  │  │  ├─ control.h               # 回差状态机 + 手动/自动模式
│  │  │  ├─ esp8266.h               # AT 指令封装、透传行缓冲
│  │  │  ├─ protocol.h              # JSON 组包/解包
│  │  │  ├─ eeprom.h                # AT24C02 读写阈值
│  │  │  └─ lcd_show.h              # LCD 显示刷新
│  │  └─ Src/                       # 同名 .c
│  ├─ Middlewares/                  # cJSON 等第三方库
│  ├─ Drivers/                      # HAL 库与 BSP（含官方 adc3 / lsens / beep / 24cxx）
│  ├─ .ioc                          # CubeMX 工程文件
│  └─ Projects/MDK-ARM/             # Keil 工程
├─ qtapp/                           # Qt 上位机
│  ├─ CMakeLists.txt
│  ├─ src/                          # MainWindow / NodeServer / NodeModel / ChartWidget /
│  │                                # ControlPanel / TtsService / AsrService / LlmClient /
│  │                                # CommandEngine / Config
│  └─ resources/
├─ tools/
│  ├─ simulate_node.py              # 虚拟节点模拟器（多点位演示）
│  └─ md2docx.py                    # 由 Markdown 生成规范格式的 docx
└─ README.md
```

### 运行形态

- **STM32**：上电自启动，无操作系统，主循环 + 定时器中断调度，IWDG 常开
- **Qt**：单进程桌面应用，监听 TCP **9092**，启动时探测 llama-server 8092
- **llama-server**：独立进程，由启动脚本拉起，Qt 仅作 HTTP 客户端
- **三者可独立启停**：llama-server 未启动时 Qt 降级为纯监控（智能交互置灰）；**ESP8266 或 Qt 全部离线时，STM32 本地闭环照常工作**

---

## 通信协议设计

### 帧格式

- 传输层：TCP；ESP8266 透传模式，Qt 用 QTcpServer（9092）接入
- 帧边界：换行符 `\n` 分隔，每帧一个 JSON 对象
- 编码：UTF-8
- 选择理由：无需长度头，调试可用串口助手直接收发；节点数据量小（每秒一帧），无粘包风险压力
- 机器可读单一事实源：动作词汇与字段校验见 `tools/protocol_schema.py`，固件 `Control_OnCommand` 与 Qt `CommandEngine::validate` 必须与之保持一致；改动任一侧后运行 `py tools/contract_test.py` 防 `get_state`/`query` 类漂移

### 消息类型

**上行（节点 → Qt）**

| type | 含义 | 关键字段 |
| --- | --- | --- |
| `report` | 周期数据上报 | node, adc, light, th, hys, relay, buzz, mode, ts |
| `ack` | 指令执行回执 | node, action, result, adc, light, relay, buzz, mode |
| `evt` | 主动事件 | node, event（`alarm` / `recover`）, adc, light |

**下行（Qt → 节点）**

| action | 含义 | 参数 | 副作用 |
| --- | --- | --- | --- |
| `relay_on` | 手动吸合继电器 | — | 切换到**手动模式** |
| `relay_off` | 手动释放继电器 | — | 切换到**手动模式** |
| `auto` | 切回自动模式 | — | 下一轮主循环按当前 `light` 重算输出 |
| `set_th` | 设阈值 | `value`（0–4095，且 > `hys`） | 立即写 EEPROM 并回 ack |
| `get_state` | 查询状态 | — | 回一条包含完整状态的 ack |
| `alarm_test` | 报警测试 | — | 蜂鸣器脉冲 1 s，**不改模式、不动执行器**，到期恢复与执行器的联动。对应 PRD 的"报警测试"按钮 |

> **`query` 不是合法动作名**，统一使用 `get_state`。三处（协议表 / System Prompt / `CommandEngine::validate`）必须一致。

### 字段约定

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `node` | int | 节点 ID（从 1 起），**所有帧必带**，多点位区分依据 |
| `adc` | int | 光照 **ADC 原始码值**（0–4095，12 位）。**数值越小表示越亮** |
| `light` | int | 相对光强指标 = `4095 - adc`。0 = 全暗，4095 = 最强。**阈值判断只用这个字段** |
| `th` | int | 当前阈值（`light` 单位） |
| `hys` | int | 当前回差（`light` 单位），满足 `0 <= hys < th` |
| `relay` | int | 继电器状态（0 = 断开，1 = 吸合） |
| `buzz` | int | 蜂鸣器状态（0 = 静音，1 = 鸣响） |
| `mode` | int | 控制模式（0 = 自动，1 = 手动） |
| `ts` | int | Unix 时间戳（秒）。节点无 RTC 时省略，**由 Qt 侧统一打时间戳** |
| `result` | string | 执行结果（`"ok"` / `"err"`） |
| `event` | string | 主动事件类型：`alarm`（越限报警）/ `recover`（回落恢复） |

### 示例帧

```json
{"node":1,"type":"report","adc":1200,"light":2895,"th":2000,"hys":150,"relay":1,"buzz":1,"mode":0}
{"node":1,"type":"cmd","action":"relay_off"}
{"node":1,"type":"cmd","action":"auto"}
{"node":1,"type":"cmd","action":"set_th","value":2500}
{"node":1,"type":"cmd","action":"get_state"}
{"node":1,"type":"ack","action":"set_th","result":"ok","adc":1200,"light":2895,"th":2500,"hys":150,"relay":0,"buzz":0,"mode":0}
{"node":1,"type":"evt","event":"alarm","adc":1200,"light":2895}
```

> 上行帧若节点无 RTC，`ts` 省略；Qt 收到后补打时间戳再入库。
> 示例自洽性：`adc=1200 → light=2895 > th=2000 → relay/buzz=1`。

### 多点位

- 每个节点烧录时配置唯一 `node_id`（本组只有一块板，实际用**编译宏**；后续多板可用 Flash 配置项）
- Qt NodeServer 维护 `QMap<int, QTcpSocket*>`，以 `node_id` 为键路由
- 节点断线：Qt 标记离线，socket 错误信号触发清理；节点侧重连后重新 report
- 虚拟节点：`tools/simulate_node.py` 用 TCP 连 Qt 的 9092，周期发 report，模拟节点 2/3

### 链路保活与离线判定

1 s 周期上报**本身就是心跳**，无需额外的 ping/pong 帧。Qt 侧对每个节点维护 `lastReportMs`：

| 判定 | 条件 | 界面表现 |
| --- | --- | --- |
| 在线 | 最近 3 s 内收到 `report` | 绿色，数据实时刷新 |
| 疑似静默失效 | socket 未断开，但 **3 s（= 3 × 上报周期）** 未收到 `report` | 黄色，数值冻结并标注"链路异常" |
| 离线 | socket `disconnected` 信号触发 | 灰色，标注"节点 N 离线" |

> 这层判定是必要的：ESP8266 透传模式下 TCP 链路**静默中断时 STM32 侧感知不到**，Qt 侧仅靠 socket 信号也不够快。

---

## 固件设计

### 模块职责

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 光照采集 | `adc_light.c/h` | PF8 模拟输入初始化、ADC3 通道 6 采样、8 次均值、`light = 4095 - adc` |
| 控制逻辑 | `control.c/h` | 执行器抽象（宏配置）、回差状态机、自动/手动模式、执行器/蜂鸣器 GPIO 控制 |
| 显示 | `lcd_show.c/h` | LCD 刷新光强值、阈值、继电器状态、模式 |
| 通信 | `esp8266.c/h` | AT 指令收发、WiFi 连接、TCP 透传、接收行缓冲、重连退避 |
| 协议 | `protocol.c/h` | cJSON 组包 report/ack/evt，解包 cmd |
| 存储 | `eeprom.c/h` | AT24C02 读写 `th`/`hys`，魔数 + 校验和，掉电记忆 |
| 主流程 | `main.c` | 初始化、1 s 定时采集、事件调度、喂狗 |

### 初始化顺序

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_IWDG_Init();                 /* ≈2 s 溢出，尽早打开 */

    delay_init(72);
    usart1_init(115200);            /* 调试串口，需插 P3 跳线 */
    lcd_init();
    led_init();  beep_init();       /* beep_init() 内部 BEEP(0)，保证上电不响 */
    LightSensor_Init();             /* 关键：使能 GPIOF 时钟 + PF8 配成模拟输入 + adc3_init() */
    Actuator_Init();                /* 执行器：当前 LED1(PE5)，目标 PD11；初始输出 = 断开 */

    EEPROM_Load(&g_th, &g_hys);     /* 魔数/校验/不变式三重校验，失败回退默认值 */
    ESP8266_Init();                 /* 失败不阻塞，本地闭环优先 */
    Control_Apply(g_th, g_hys, MODE_AUTO);

    while (1) { /* 见主循环调度 */ }
}
```

> **初始化要点**：
> - 官方 `adc3_init()` 只会把 `ADC3_CHY_GPIO_PIN`（默认宏值 = **PA1**）配成模拟输入，**不会配 PF8**。PF8 的模拟输入模式由官方 `lsens_init()` 负责（`LSENS_ADC3_CHX_GPIO_PIN = GPIO_PIN_8`）。因此本项目要么直接调用 `lsens_init()`，要么在 `LightSensor_Init()` 里自行完成 `__HAL_RCC_GPIOF_CLK_ENABLE()` + PF8 的 `GPIO_MODE_ANALOG` 配置。
> - 官方 `adc3_init()` 顺带把 PA1 配成模拟输入是无害的副作用（高阻），但建议**拔掉 P7 跳线**，避免 PA1 与 TPAD/电位器 RV1 连通后灌入微小电流。

### 主循环调度

```c
#define HEARTBEAT_TOGGLE_SEC   5      /* LED0 运行心跳：每 5 s 翻转一次（5 s 亮 / 5 s 灭） */

while (1)
{
    IWDG_Feed();

    if (flag_1s)                                  /* 定时器中断置位 */
    {
        flag_1s = 0;

        if (++g_hb_cnt >= HEARTBEAT_TOGGLE_SEC) {  /* 心跳：程序没卡死的最直观证据 */
            g_hb_cnt = 0;
            LED0_TOGGLE();
        }

        g_adc   = Light_ReadRaw(ADC_LIGHT_AVG_TIMES);   /* 8 次均值，≈0.17 ms */
        g_light = LIGHT_FROM_ADC(g_adc);                /* 4095 - adc */
        LCD_ShowLight(g_light, g_th, g_relay, g_mode);
        Protocol_SendReport(g_node_id, g_adc, g_light, g_th, g_hys, g_relay, g_buzz, g_mode);
    }

    Esp8266_Poll();                               /* 非阻塞：AT 状态机 + 接收行缓冲 */
    while (Protocol_PopCommand(&cmd))             /* 取出完整帧再执行 */
        Protocol_ParseAndExecute(&cmd);

    Control_Loop(g_light);                        /* 回差 + 手动优先，每次主循环都跑 */
}
```

> **两颗灯分工不重叠**：LED0 只做心跳（每 5 s 翻转），LED1 由执行器抽象驱动、只表示执行器通断。不要用同一颗灯兼两种语义，否则演示时无法判断"灯亮"是程序活着还是执行器吸合。

> **禁止**在主循环里调用 `HAL_Delay` 或不喂狗的长等待。ESP8266 的 AT 等待一律用"带 `IWDG_Feed()` 的有限轮询"实现。

### 采样与滤波

```c
/* 官方 adc3_get_result() 内部已固定：ADC3 + 采样时间 239.5 周期 + 软件触发，单次 ≈21 µs */
#define ADC_LIGHT_AVG_TIMES   8

static uint16_t Light_ReadRaw(uint8_t times)
{
    uint32_t sum = 0;
    for (uint8_t i = 0; i < times; i++)
        sum += adc3_get_result(ADC_CHANNEL_6);   /* PF8 = ADC3_IN6 */

    return (uint16_t)(sum / times);
}
```

> **不要直接用官方 `adc3_get_result_average(ch, times)`**：它在每次采样后调用 `delay_ms(5)`，8 次会**阻塞约 40 ms**；而光照变化缓慢，无需这种间隔。自写循环 8 次仅 ≈0.17 ms。
> `ADC_CHANNEL_6` 即官方 `lsens.h` 中的 `LSENS_ADC3_CHX`，可直接引用该宏以免两处不一致。
> **不要使用 `lsens_get_val()`**：它返回 0–100 的压缩值（分辨率仅 2.44%），与协议声明的 0–4095 不符。

### 回差控制与手动优先状态机

```c
typedef enum { MODE_AUTO = 0, MODE_MANUAL = 1 } ctrl_mode_t;

static uint16_t g_th  = TH_DEFAULT;      /* 2000 */
static uint16_t g_hys = HYS_DEFAULT;     /*  150 */
static uint8_t  g_mode = MODE_AUTO;
static uint8_t  g_relay = 0, g_buzz = 0;

/* 输出统一出口：任何路径改输出都走这里，避免继电器/蜂鸣器状态漂移 */
static void Control_ApplyOutput(uint8_t on)
{
    g_relay = on;
    g_buzz  = on;                    /* 任务书要求二者联动，见 BUZZ_FOLLOW_RELAY */
    Actuator_Set(on);                /* 执行器：当前 LED1，目标继电器 PD11，业务代码不变 */
    Buzzer_Set(on);
}

void Control_Apply(uint16_t th, uint16_t hys, uint8_t mode)
{
    g_th = th; g_hys = hys; g_mode = mode;
}

/* 自动模式下的目标输出（回差在这里体现） */
static uint8_t Control_AutoTarget(uint16_t light)
{
    if (light > g_th)                     return 1;   /* 光过强 → 遮阳/报警 */
    if (light < (uint16_t)(g_th - g_hys)) return 0;   /* 回落到回差以下才释放 */
    return g_relay;                                   /* 死区：保持 */
}

void Control_Loop(uint16_t light)
{
    if (g_mode != MODE_AUTO)
        return;                                       /* 手动模式：自动逻辑完全不介入 */

    uint8_t target = Control_AutoTarget(light);
    if (target != g_relay) {
        Control_ApplyOutput(target);
        Protocol_SendEvent(target ? "alarm" : "recover", light);
    }
}

/* 下行指令入口：手动操作即进入手动模式，这就是"手动优先" */
void Control_OnCommand(const char *action, int32_t value)
{
    if (!strcmp(action, "relay_on")) {
        g_mode = MODE_MANUAL; Control_ApplyOutput(1); Protocol_SendAck(action, "ok");
    } else if (!strcmp(action, "relay_off")) {
        g_mode = MODE_MANUAL; Control_ApplyOutput(0); Protocol_SendAck(action, "ok");
    } else if (!strcmp(action, "auto")) {
        g_mode = MODE_AUTO;                           /* 下一轮 Control_Loop 按当前 light 重算 */
        Protocol_SendAck(action, "ok");
    } else if (!strcmp(action, "set_th")) {
        if (value < 0 || value > 4095 || (uint16_t)value <= g_hys) {
            Protocol_SendAck(action, "err");          /* 阈值必须大于回差，否则 th-hys 下溢 */
            return;
        }
        g_th = (uint16_t)value;
        EEPROM_Save(g_th, g_hys);                     /* ≤5 ms 写周期，写完再回 ack */
        Protocol_SendAck(action, "ok");
    } else if (!strcmp(action, "get_state")) {
        Protocol_SendAck(action, "ok");               /* ack 已携带完整状态 */
    } else {
        Protocol_SendAck(action, "err");              /* 未知动作一律拒绝 */
    }
}
```

> **为什么必须有 `mode`**：[PRD.md](PRD.md) 要求"Qt 手动控制可随时覆盖自动状态"。若没有该标志，Qt 下发 `relay_off` 后，下一轮 `Control_Loop` 立即按 `light > th` 把继电器重新吸合，用户看到继电器"弹一下就回去"。
> **不变式**：`0 <= hys < th <= 4095`。该不变式在 EEPROM 加载、`set_th` 校验两处强制，是防止 `g_th - g_hys` 下溢（unsigned 下溢后比较永假、继电器锁死）的唯一屏障。

### 阈值存储

AT24C02（I2C1，7 位地址 `0x50`），**8 字节布局**，恰好等于该芯片的一页写入长度，无需跨页处理：

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0x00 | 2 | 魔数 `0x47 0x48`（`'G' 'H'`） |
| 0x02 | 2 | `th`（大端） |
| 0x04 | 2 | `hys`（大端） |
| 0x06 | 2 | 校验和 = 前 6 字节之和取低 16 位 |

**上电加载流程**：

```c
void EEPROM_Load(uint16_t *th, uint16_t *hys)
{
    uint8_t buf[8];
    *th = TH_DEFAULT; *hys = HYS_DEFAULT;             /* 先落默认值 */

    if (!AT24C02_Read(0x00, buf, sizeof(buf)))        return;   /* 读失败 */
    if (buf[0] != 0x47 || buf[1] != 0x48)             return;   /* 魔数不符（空片全 0xFF） */
    if (Checksum16(buf, 6) != ((uint16_t)buf[6] << 8 | buf[7])) return;  /* 校验失败 */

    uint16_t t = ((uint16_t)buf[2] << 8 | buf[3]);
    uint16_t h = ((uint16_t)buf[4] << 8 | buf[5]);
    if (h >= t || t > 4095) return;                    /* 不变式不满足，回退默认值 */

    *th = t; *hys = h;
}
```

> 魔数的作用：空片读出全 `0xFF`，旧的"6 字节无魔数"布局靠校验和凑巧失败来兜底，属于侥幸；加魔数后语义明确，后续扩展字段也不会误判。

### 接收行缓冲（透传模式的关键细节）

`AT+CIPMODE=1` 进入透传后，**收到的数据不再带 `+IPD` 前缀**，ESP8266 只是把串口收到的字节当作网络数据透传。因此固件必须自己做行缓冲：

```c
#define RX_RING_SIZE   256      /* 环形缓冲 */
#define RX_FRAME_MAX   200      /* 单帧上限 */
#define RX_DISCARD_MAX 512      /* 连续无 '\n' 超过此长度即丢弃，防撑爆 */

/* 串口中断只做一件事：把字节塞进环形缓冲 */
void USART2_IRQHandler(void)
{
    if (LL_USART_IsActiveFlag_RXNE(USART2)) {
        uint8_t b = LL_USART_ReceiveData8(USART2);
        Ring_Push(&g_rx, b);
    }
    HAL_UART_IRQHandler(&huart2);
}

/* 主循环里非阻塞地取完整帧 */
bool Protocol_PopCommand(char *out, size_t cap)
{
    int c;
    while ((c = Ring_Pop(&g_rx)) >= 0) {
        if (c == '\n') {
            if (g_line_len == 0) continue;                     /* 空行忽略 */
            g_line[g_line_len] = '\0';
            strncpy(out, g_line, cap - 1); out[cap - 1] = '\0';
            g_line_len = 0;
            return true;
        }
        if (g_line_len < RX_FRAME_MAX - 1) {
            g_line[g_line_len++] = (char)c;
        } else {
            g_line_len = 0;                                     /* 超长：丢弃并复位 */
            g_drop_cnt++;
        }
    }
    return false;
}
```

> 超长无 `\n` 的保护必须有：一旦对端持续发无换行数据，没有上限的行缓冲会溢出。

### ESP8266 AT 状态机

```text
IDLE → RST → CWMODE=1 → CWJAP → CIPSTART("TCP","<Qt_IP>",9092) → CIPMODE=1(透传) → CONNECTED
                                                                            ↓
DISCONNECT ←────────────────────────────────────────────── 网络断开 / 错误 / AT 超时
     ↓ 退避重连（2 s → 4 s → 8 s，上限 30 s，带 IWDG_Feed）
   RST 重新开始
```

- 每个 AT 步骤都有超时，超时即回退，绝不无限等待
- 透传模式下若链路静默断开，STM32 侧收不到通知；**靠 1 s 上报失败（发送超时/无 ack）触发重连**
- Qt 的 IP 与端口来自配置文件（不硬编码）

### 看门狗

- IWDG 溢出周期约 2 s（LSI ≈ 40 kHz，预分频 64 → 625 Hz，重装值 1250 → 2.0 s）
- 主循环顶部喂狗；`Esp8266_Poll()` 的每个等待步进内也喂狗
- 验收要求"连续运行 30 分钟无死机"，IWDG 是这条要求最直接的保障手段

---

## Qt 上位机设计

### 模块职责

| 模块 | 类 | 职责 |
| --- | --- | --- |
| 网络服务 | `NodeServer` | QTcpServer 监听 **9092**，多节点 socket 管理，行分隔 JSON 解析，离线判定 |
| 数据模型 | `NodeModel` | `QMap<int, NodeState>` 存各节点 adc/light/th/hys/relay/buzz/mode/online/lastReportMs |
| 曲线 | `ChartWidget` | QChart 实时光强曲线（每节点一条线，最近 300 点） |
| 控制面板 | `ControlPanel` | 开关继电器、"切回自动"、阈值输入、报警测试；显示当前模式 |
| 语音合成 | `TtsService` | QTextToSpeech 封装，中文播报，运行时枚举语音 |
| 语音识别 | `AsrService` | 以 `QProcess` 起 PowerShell + `System.Speech` 听写，识别结果经信号回主线程 |
| 推理客户端 | `LlmClient` | QNetworkAccessManager 调 llama.cpp API，**工作线程内构造与使用** |
| 指令引擎 | `CommandEngine` | 三路指令统一校验、组装 cmd、经 NodeServer 下发 |
| 配置 | `Config` | QSettings 读写 |

### 线程模型

```text
主线程（UI 事件循环）
 ├─ NodeServer（QTcpServer + socket 信号槽，Qt 事件循环天然异步）
 ├─ ChartWidget / ControlPanel / TtsService / AsrService（均为 UI 线程对象）
 └─ CommandEngine（校验与下发，轻量、非阻塞）
        │ 请求 / 结果 通过信号槽跨线程传递
        ▼
工作线程（QThread，run() 内 exec()）
 └─ LlmClient + QNetworkAccessManager（HTTP 全异步，不阻塞、不用 QEventLoop）
```

- LLM 调用必须离开主线程：HTTP 等待数秒，留在主线程会冻结界面
- NodeServer 收发走 Qt 信号槽异步，不占工作线程
- ASR 识别结果经信号回主线程，再决定是否送 LLM
- **线程归属**：`LlmClient` 与其内部 `QNetworkAccessManager` 必须创建、使用、销毁在同一个工作线程。不要在主线程 `new` 完再 `moveToThread`；推荐用工作线程的 `started` 信号连接一个工厂槽，在槽内构造。

### LlmClient 实现示意

```cpp
class LlmClient : public QObject
{
    Q_OBJECT
public:
    explicit LlmClient(QObject *parent = nullptr)
        : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

public slots:
    void ask(const QString &userText, const NodeState &st);
    void configure(const QString &url, const QString &model, const QString &token);

signals:
    void finished(const QString &content);   // 跨线程自动排队回主线程
    void failed(const QString &reason);

private:
    QNetworkAccessManager *m_nam;
    QString m_url, m_model, m_token;
    QJsonArray buildMessages(const QString &userText, const NodeState &st) const;
};

void LlmClient::ask(const QString &userText, const NodeState &st)
{
    QNetworkRequest req{QUrl(m_url)};                 // http://127.0.0.1:8092/v1/chat/completions
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(15000);                    // Qt 5.15+ 内置超时，无需自建定时器
    if (!m_token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());

    QJsonObject body{
        {"model",       m_model},                     // 取值必须来自 GET /v1/models
        {"temperature", 0.1},                         // 控制类任务需稳定输出
        {"max_tokens",  512},                         // 思考型模型需留足，太小会导致 content 为空
        {"stream",      false},                       // 一次性返回更易解析
        // 关闭思考：llama.cpp 较新版本支持该字段；若本机版本不支持会报错，请删除此键，
        // 改为在 System Prompt 中要求"直接给最终 JSON"，并把 max_tokens 调大后重新实测
        {"chat_template_kwargs", QJsonObject{{"enable_thinking", false}}},
        {"messages",    buildMessages(userText, st)}
    };

    QNetworkReply *rep = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            emit failed(QStringLiteral("推理服务不可用：%1").arg(rep->errorString()));
            return;
        }
        const QByteArray raw = rep->readAll();
        QString content;
        if (!ExtractContent(raw, &content)) {         // 取 choices[0].message.content
            emit failed(QStringLiteral("响应格式异常：缺少 choices[0].message.content"));
            return;
        }
        emit finished(content);
    });
}
```

> **相比早期的"`QThread::create` + 局部 `QNetworkAccessManager` + 嵌套 `QEventLoop`"写法**，本方案不手工跑事件循环、不跨线程管理 `QNetworkAccessManager` 生命周期、不硬编码 Token，直接用 Qt 的异步信号槽与内置传输超时，更不易出并发问题。

### System Prompt

```text
你是智能温室控制助手。根据用户指令，只输出 JSON，不要输出任何其他文字，不要解释。

继电器语义：光照过强时吸合继电器遮阳/报警，光照恢复正常后释放。

可用动作：
  {"action":"relay_on"}            手动吸合继电器（进入手动模式）
  {"action":"relay_off"}           手动释放继电器（进入手动模式）
  {"action":"auto"}                切回自动模式
  {"action":"set_th","value":N}    将光强阈值设为 N（0-4095，且必须大于回差 150）
  {"action":"get_state"}           查询当前状态

当前状态：节点1 光强 {light}（0 最暗 / 4095 最强），阈值 {th}，继电器 {relay}，模式 {mode}。

示例：
用户：光太强了，把遮阳帘放下来
助手：{"action":"relay_on"}

用户：把阈值调到 2500
助手：{"action":"set_th","value":2500}

用户：现在光照多少
助手：{"action":"get_state"}
```

> **提示词与协议、固件三处必须一致**：① 动作名统一用 `get_state`（不要写成 `query`）；② 示例语义与固件的 `light > th` 判断保持一致 —— 用"光太强了 → `relay_on`（遮阳）"，**不要写成"太暗了开灯"**。

### 指令引擎校验规则

```cpp
bool CommandEngine::validate(const QJsonObject &cmd, QString *why)
{
    const QString action = cmd.value("action").toString();

    if (action == "relay_on" || action == "relay_off"
        || action == "auto"   || action == "get_state")
        return true;

    if (action == "set_th") {
        if (!cmd.contains("value") || !cmd.value("value").isDouble()) {
            *why = QStringLiteral("set_th 缺少数值参数 value");
            return false;
        }
        const int v = cmd.value("value").toInt(-1);
        if (v < 0 || v > 4095) {
            *why = QStringLiteral("阈值 %1 超出 ADC 量程 0-4095").arg(v);
            return false;
        }
        if (v <= m_hys) {
            *why = QStringLiteral("阈值 %1 必须大于回差 %2").arg(v).arg(m_hys);
            return false;
        }
        return true;
    }

    *why = QStringLiteral("未知动作：%1").arg(action);
    return false;                                  // 未知动作一律拒绝，不下发
}
```

> 三路指令（按钮 / 语音 / 大模型）**走同一个** `validate()`。UI 侧不要自己再写一套判断，避免规则分叉。

### 配置项（QSettings）

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `tcp/port` | **9092** | Qt 监听端口，与节点 `CIPSTART` 目标一致，**必须与 llama-server 的 8092 错开** |
| `tcp/bindAddress` | `0.0.0.0` | Qt 监听地址。**必须能被局域网上的 ESP8266 访问**——节点是独立设备，Qt 若只绑 `127.0.0.1` 将永远连不上；需收紧时改为 PC 的 LAN IP |
| `llm/url` | `http://127.0.0.1:8092/v1/chat/completions` | 推理 API 地址 |
| `llm/model` | `glm-4.7-flash` | 必须存在于 `GET /v1/models` 返回列表 |
| `llm/token` | （空） | 为空时不发送 `Authorization` 头 |
| `llm/timeoutMs` | 15000 | HTTP 超时 |
| `tts/enabled` | true | 播报开关 |
| `tts/voice` | `Microsoft Huihui` | 运行时用 `availableVoices()` 校验，找不到则回退第一个 zh-CN 语音 |
| `asr/locale` | `zh-CN` | 识别语言 |
| `chart/maxPoints` | 300 | 曲线保留点数 |
| `node/offlineTimeoutMs` | 3000 | 静默失效判定阈值（3 × 上报周期） |

### 语音链路降级

| 故障 | 检测方式 | 降级行为 |
| --- | --- | --- |
| 未安装 Qt TextToSpeech 模块 | 编译期失败 | **必须先补装模块**，否则 P6 无法交付 |
| 无可用中文语音 | `availableVoices()` 为空或无 zh-CN | 播报开关置灰，状态栏提示"未找到中文语音" |
| 识别不可用 | 无 powershell / 缺 System.Speech / 无音频设备 | "说话"按钮置灰，保留文字输入框，链路其余部分照常 |

---

## 关键流程

### 启动与连接

1. 用户双击 `启动.cmd`，脚本检测 **8092** 端口，未启动则拉起 llama-server，等待 `/health` 返回 ok
2. 用 `状态.cmd` 或 `curl http://127.0.0.1:8092/v1/models` 确认 `<model>` 标识
3. 用户启动 Qt 应用，NodeServer 监听 **9092**
4. Qt 探测 8092：`/health` 不通则界面提示"大模型功能不可用"并置灰智能交互，**监控与控制功能仍可用**
5. STM32 上电，ESP8266 连 WiFi 并按配置的 `<PC_IP>:9092` 建 TCP，进入透传
6. Qt 收到首个 `report`，标记节点 1 在线，曲线开始绘制
7. **演示前预热**：先发一次 `get_state` 类请求（或任何一次 LLM 调用），把模型权重加载完成，避免演示时首次请求卡十几秒

### 语音控制继电器（正确语义）

1. 用户点"说话"按钮，AsrService 开始识别
2. ASR 返回文本"光太强了，把遮阳帘放下来"，显示在对话区
3. CommandEngine 组 messages（System Prompt + 当前 `light/th/relay/mode` + 用户文本），交 LlmClient
4. LlmClient 工作线程 POST llama-server，拿到 `{"action":"relay_on"}`
5. `validate()` 通过 → NodeServer 下发 `{"node":1,"type":"cmd","action":"relay_on"}`
6. STM32 执行 `Control_ApplyOutput(1)` 并切到手动模式，回 `ack`
7. Qt 收 `ack`，更新界面状态与模式标签，TtsService 播报"已放下遮阳帘，当前光强 2895"

### 手动优先流程

```text
自动模式报警中（relay=1）
   │  用户在 Qt 点"关继电器"
   ▼
下发 relay_off → 固件 mode=MANUAL，输出 0，回 ack(mode=1)
   │  ← 关键：此后 light 再高，Control_Loop 直接 return，不会回弹
   ▼
用户点"切回自动" → 下发 auto → 固件 mode=AUTO，回 ack(mode=0)
   │
   ▼
下一轮 Control_Loop 按当前 light 重算输出（可能立即又吸合，这是预期行为）
```

### 断线重连

- **ESP8266 侧**：AT 指令超时或收到 `CLOSED` → 状态机回退，退避重连（2 s → 4 s → 8 s，上限 30 s），每个等待步进喂狗
- **Qt 侧**：socket `disconnected` 信号 → NodeModel 标记节点离线，界面显示"节点 1 离线"
- **Qt 侧**：socket 未断开但 3 s 无 `report` → 标记"链路异常"（覆盖透传模式下的静默失效）
- **Qt 侧**：llama-server 探测失败 → 智能交互按钮置灰，不影响本地监控
- **STM32 侧**：ESP8266 全挂也不影响本地采集 / 显示 / 报警，恢复后自动续传 `report`

---

## 非功能设计

### 实时性

| 环节 | 指标 | 依据 |
| --- | --- | --- |
| 单次 ADC 转换 | 252 个 ADC 周期 = **21 µs** | 采样 239.5 + 转换 12.5，ADCCLK 12 MHz |
| 8 次均值滤波 | **≈ 0.17 ms**（自写循环，无延时） | 8 × 21 µs |
| 本地闭环（采样 → 判定 → 输出） | **< 100 ms**，实测应在 1 ms 量级 | 控制逻辑在主循环每次执行，不等网络 |
| 远程数据刷新 | 1 s（与上报周期一致） | — |
| 大模型语音链路端到端 | **暂不承诺具体秒数，需实测后填入** | 见下方说明 |

> **关于大模型延迟**：思考型模型的 reasoning token 远超控制指令本身，"回复约 50 token" 这类估算不成立；同时 `max_tokens` 过小会导致 `content` 为空（内容全在 reasoning）。因此 **不在设计文档里写未经验证的秒数**，改为在 M5 阶段用真实请求实测（记录 prompt tokens / completion tokens / 端到端耗时，注明是否关闭思考、`max_tokens` 取值），实测结果再填入报告。可复现性要求：报告中的性能数字必须附上测量命令与参数。
>
> 同理，"主用 glm-4.7-flash"的结论需要有可复现的对比数据支撑（建议 `llama-bench` 或 server 日志），而不是只给一张没有测法的表。

### 可靠性

- **本地自治**：断网 / 上位机未启动 / 大模型不可用，STM32 的采集、显示、报警闭环全部照常
- ESP8266 断线自动重连，退避策略防风暴
- 指令执行必须回 `ack`，Qt 收到 `ack` 才更新"已执行"状态
- 阈值 EEPROM 存魔数 + 校验和 + 不变式校验，任一项失败即用默认值，不影响启动
- IWDG ≈ 2 s，主循环与 AT 等待路径均喂狗
- Qt 启动时检测 llama-server，不可用时降级而非崩溃

### 安全性

- **llama-server 绑定 `127.0.0.1`**：只服务本机的 Qt，绝不对外
- **Qt NodeServer 必须监听局域网网卡**：ESP8266 是**局域网上的独立设备**，Qt 若只绑回环，节点永远连不上。默认 `0.0.0.0`（或指定 PC 的 LAN IP），并用 Windows 防火墙把 9092 的入站限制在内网
- **端口隔离**：llama-server = 8092，Qt = 9092
- `set_th` 参数范围校验（0–4095 且 > `hys`），非法值拒绝并提示，不静默截断
- 大模型输出必须经 `CommandEngine` 校验后才下发
- WiFi SSID/密码走不提交的 `wifi_config.h`（仓库内只提交 `wifi_config.h.example`），或 AT 配网
- 演示阶段协议无鉴权；生产环境可加节点 Token 与指令序号防重放（未排期）

### 性能

- 曲线保留最近 300 点，避免内存增长
- LLM 响应一次性解析（非 stream），控制指令短
- 固件 ROM/RAM 占用远低于 512 KB / 64 KB 上限，实测值在 M7 冻结时记录并填入报告

### 可维护性

- 固件按模块分文件，ADC / 控制 / 通信解耦，可单独测试（如注释 ESP8266 模块跑纯本地闭环）
- **输出统一出口** `Control_ApplyOutput()`，避免执行器与蜂鸣器状态在多处被改写而漂移
- **执行器抽象**（见 A.5）：执行器引脚与有效电平集中在 4 行宏内，更换硬件不影响状态机、通信协议与上位机
- Qt 指令引擎统一入口，新增控制动作只需加 `action` 分支；三路指令共用同一 `validate()`
- 协议 JSON 可扩展，新增字段不影响旧节点（cJSON / QJson 忽略未知键）
- 引脚分配集中在本文件附录 A，其余文档引用之，避免多头维护

---

## 环境与配置

### 开发环境（已验证可用）

| 项 | 值 | 核验状态 |
| --- | --- | --- |
| 操作系统 | Windows 11 | — |
| Qt | 6.12.0（`E:\Qt`），MinGW 13.1.0 64-bit | ✅ 已核验 |
| Qt 模块 | QtCharts / QtNetwork / QtSql / QtMultimedia 已安装 | ✅ 已核验 |
| **Qt TextToSpeech** | **未安装** | ❌ **需用 MaintenanceTool 补装** |
| Qt Creator | 20.0.2 Enterprise | — |
| 固件工具 | Keil MDK 或 CLion + arm-none-eabi-gcc | — |
| 推理引擎 | llama.cpp Server，`127.0.0.1:8092`，router 模式 | ✅ 已核验启动脚本 |
| Python | **`python` 命令不可用（Store 占位 stub）；用 `py`（3.14.7）** | ✅ 已核验 |
| GPU | RTX 4060 Laptop 8GB | — |
| 中文 TTS 语音 | SAPI5 仅 `Microsoft Huihui Desktop`；OneCore 另有 Huihui/Kangkang/Yaoyao | ✅ 已核验（**无 Xiaoxiao**） |

### 运行配置

- WiFi：ESP8266 与 PC 同一局域网，SSID/密码走 `wifi_config.h` 或 AT 配网
- **端口**：llama-server 8092；Qt 监听 9092；节点 `CIPSTART` 目标为 `<PC_IP>:9092`
- **PC 侧准备**：Windows 防火墙放行 9092 入站；PC 建议设静态 IP 或固定 DHCP 保留，避免节点配置失配
- 阈值默认：`th = 2000`，`hys = 150`（`light` 单位，按实际光照环境调整）

### 演示环境

- 光源：手电筒或手机闪光板（模拟光照增强 → `light` 上升、`adc` 下降）
- 遮挡：手掌或纸板（模拟光照减弱 → `light` 下降、`adc` 上升）
- 多点位：`py tools/simulate_node.py --id 2` 模拟节点 2
- 执行器：**先验实验阶段**以板载 **LED1（绿）** 指示执行器通断；**借用继电器到位后**切换宏并观察真实触点动作。**LED0（红）每 5 s 翻转**作运行心跳
- 断网演示：直接拔掉 ESP8266 的 VCC 或断开热点，展示本地闭环仍在工作（加分项）

---

## 开发里程碑

| 里程碑 | 内容 | 验收判据 |
| --- | --- | --- |
| **M1 本地感知** | ADC3 通道 6 读 PF8、8 次均值、`light` 换算、LCD 显示 | 手电照射 `light` 上升、遮挡下降，方向正确 |
| **M2 本地闭环** | 回差状态机 + 手动优先、执行器 + 蜂鸣器联动、AT24C02 掉电记忆、IWDG、LED0 心跳 | 阈值边界三点行为正确；断电重启阈值保留；上电执行器断开、蜂鸣器不响（**阶段一执行器为 LED1 等效负载**，见 A.5） |
| **M2b 执行器切换** | 借用继电器到位后：替换 `ACTUATOR_*` 四行宏、接 VCC/GND/IN 三线、按 T17 复测 | 真实继电器完成 T02/T03/T05，且状态机 / 协议 / 上位机**零改动** |
| **M3 无线通信** | ESP8266 连 WiFi、TCP 透传、行缓冲收包、JSON report 上报 | Qt 收到数据；拔网线后本地闭环不受影响 |
| **M4 Qt 控制** | NodeServer(9092)、ControlPanel 按钮、改阈值、切回自动、QChart 曲线 | 命令下发 → 执行 → ack 回读 → 界面更新；离线判定生效 |
| **M5 大模型接入** | LlmClient 工作线程调 llama.cpp API、System Prompt、JSON 解析、CommandEngine 校验 | 文字输入控继电器；**实测并记录端到端延迟与 token 数** |
| **M6 语音交互** | 补装 Qt TextToSpeech、TTS 播报、ASR 识别、三路指令统一 | 语音 → LLM → 继电器全链路演示，TTS 正确播报 |
| **M7 多点位与优化** | node_id 多节点、虚拟节点模拟器、断线重连、异常提示、降级处理、演示版冻结 | 记录固件 ROM/RAM 占用；连续运行 30 分钟无死机 |
| **M8 报告与答辩** | 设计报告（参考模板）、答辩 PPT 与演示脚本 | 报告格式符合模板要求；演示脚本可复现 |

---

## 待确认事项

| # | 事项 | 状态 / 影响 |
| --- | --- | --- |
| 1 | **Qt TextToSpeech 模块补装** | ❌ **阻塞 M6**（本机未安装，任务书第 3 条要求"播报"） |
| 2 | **借用继电器的到位时间** | 🤝 借用渠道已确认，**非阻塞**：先验实验用板载 LED1 等效负载（A.5）先把控制逻辑验证通过；借用件到位后按 A.6 预留的接口即插即用。需尽早与出借方确认具体日期，以便安排 T17 复测与答辩演示 |
| 3 | ESP8266 是否已刷 AT 固件、供电方案（独立 3.3 V/1 A 还是板载 AMS1117 + 电容） | ⚠️ ESP8266 峰值电流约 300 mA，板载 LDO 常不够，会反复重启 |
| 4 | 借用继电器的触发电平（高/低）与型号 | ⚠️ **建议选低电平触发**（3.3 V 灌电流可靠；LED1 也是低电平有效，切换时 `ACTUATOR_ACTIVE_LEVEL` 无需改动）。借用件到手后实测确认并填入 A.5 |
| 5 | PC 的局域网 IP 与防火墙设置 | ⚠️ 节点 `CIPSTART` 目标，建议固定 IP + 放行 9092 |
| 6 | llama.cpp 版本是否支持 `chat_template_kwargs` | ⚠️ 决定关闭思考的写法；不支持则改用 Prompt 约束 + 放大 `max_tokens` |
| 7 | 光敏 ADC 与实际光照（lux）的对应关系 | 可选加分：标准照度计两点标定后线性拟合 |
| 8 | 大模型端到端延迟实测值 | ⚠️ 报告需要，M5 阶段补 |
| 9 | **设计报告模板** | ⚠️ 项目目录中未找到，需向指导老师索取 |
| 10 | `hys` 是否需要远程可配 | 当前固定 150，`set_th` 只改 `th`；如需远程改回差需扩展协议 |
| 11 | 是否支持连续对话上下文 | 当前单轮解析，未排期 |
| 12 | 周榜/月榜等统计功能 | 未排期（当前仅实时曲线与最近 300 点） |

---

## 附录 A：引脚分配（唯一事实来源）

### A.1 引脚分配表

| 功能 | 引脚 | 外设 / 模式 | 电平与备注 |
| --- | --- | --- | --- |
| 光照采集 | **PF8** | ADC3_IN6，模拟输入 | 板载 LS1 光敏电阻，硬连接；插 3.5″ 屏时依然干净。**唯一数据源** |
| 报警蜂鸣器 | **PB8** | GPIO 推挽输出 | 板载有源蜂鸣器，**高电平响**（官方 `beep.c`：`BEEP(0)` = 关闭） |
| 报警执行器（目标） | **PD11** | GPIO 推挽输出 | 外接继电器模块，板载未接任何器件。**低电平触发优先**，见 A.3 |
| 报警执行器（当前） | **PE5**（板载 LED1，绿） | GPIO 推挽输出 | **等效负载**：低电平点亮，与目标继电器的有效电平一致，见 A.5 |
| LCD | FSMC 16 位并口 | PD0 / PD1 / PD4 / PD5 / PD8–PD10 / PD14 / PD15、PE7–PE15、PG0 / PG12、PB0（背光） | 3.5″ NT35310，480×320，MCU 屏。**这些脚一律不得挪用** |
| ESP8266 TX→MCU RX | **PA3** | USART2_RX | 与 RS485 共用，由 P5 跳线选择 |
| ESP8266 RX←MCU TX | **PA2** | USART2_TX | 同上 |
| ESP8266 供电 | 3.3 V | — | ⚠️ 建议独立 3.3 V/1 A + 100 µF，见 §硬件注意 |
| 阈值存储 | **PB6 / PB7** | I2C1 SCL / SDA | 板载 AT24C02，硬件 4.7 K 上拉，硬连接**不可复用** |
| 运行心跳 | **PB5**（板载 LED0，红） | GPIO 推挽输出 | **每 5 s 翻转一次**（5 s 亮 / 5 s 灭）；板载 LED 低电平点亮 |
| 调试串口 | PA9 / PA10 | USART1 + CH340C | 需插 **P3** 跳线 |
| 下载调试 | PA13 / PA14 | SWD | **绝不用作普通 IO**（禁用后无法仿真） |
| 独立按键 | PE4 / PE3 / PE2 / PA0 | GPIO 输入 | KEY0 / KEY1 / KEY2 / KEY_UP（可用于本地消音，可选） |
| **本项目不用** | **PA1** | TPAD / 电位器 RV1（P7 跳线） | 与光照无关。建议**拔掉 P7 跳线** |

### A.2 光照方向与阈值语义（务必牢记）

```text
光照增强 → LS1 阻值下降 → PF8 电压下降 → ADC 码值减小
```

**依据**：官方 `lsens.c` 的 `return (uint8_t)(100 - adc3_get_result_average(...) / 40)`，其头文件注释为 `0,最暗; 100,最亮` —— 亮时原始 ADC ≈ 0。

> 官方 `实验20` 的 `readme.txt` 有两处容易误导：
> ① 实验现象段写成 `通道6(PA1)`，实为 **PF8**（笔误）；
> ② "光线越亮，值越大" 说的是**换算后的 0–100 值**，不是原始 ADC。
> 另注：官方 `adc3.h` 里 `ADC3_CHY_GPIO_PIN = GPIO_PIN_1`、`ADC3_CHY = ADC_CHANNEL_1` 是**残留默认值**，与光敏无关；光敏真正的通道号在 `lsens.h` 的 `LSENS_ADC3_CHX = ADC_CHANNEL_6`。

为让"光照超过阈值就报警"在全项目语义一致，定义：

```c
#define LIGHT_ADC_MAX        4095
#define LIGHT_FROM_ADC(a)    ((uint16_t)(LIGHT_ADC_MAX - (a)))
#define TH_DEFAULT           2000    /* light 单位 */
#define HYS_DEFAULT          150     /* light 单位 */
```

**阈值与回差**：

| 条件 | 行为 |
| --- | --- |
| `light > th` | 继电器吸合 + 蜂鸣器响 + 上报 `evt: alarm` |
| `light < th - hys` | 继电器释放 + 蜂鸣器停 + 上报 `evt: recover` |
| `th - hys <= light <= th` | 死区，保持当前状态 |

**不变式**：`0 <= hys < th <= 4095`，在 EEPROM 加载与 `set_th` 校验两处强制。

### A.3 执行器驱动电路

**情形一：成品单路继电器模块（本项目采用借用品）**

```text
模块 VCC ── 独立 5 V
模块 GND ── STM32 GND（必须共地）
模块 IN  ── STM32 PD11
```

- **成品模块内部已自带驱动三极管、光耦和续流二极管**，因此**不需要**外接 S8050 / 1N4007 / 1 kΩ 电阻，接三根线即可
- **优先选用低电平触发**（模块上一般有高/低电平跳线）：
  - 低电平触发时，光耦 LED 由模块的 5 V 经限流电阻供电，PD11 只需**灌电流**，STM32 的 20 mA 灌能力绰绰有余，3.3 V 逻辑完全可靠；
  - 高电平触发时，要靠 PD11 的 **3.3 V** 去驱动按 5 V 设计的限流电阻，光耦电流仅约 2.3 mA，可能推不饱三极管 → 继电器吸合不稳或不吸合；
  - 若只能拿到高电平触发型，加一只 S8050 做电平转换。
- 线圈供电单独走 5 V，不要从板载 3.3 V 取电

**情形二：裸继电器（无模块，才需要）**

```text
STM32 PD11 ──[1 kΩ]──┬── S8050 (NPN) 基极   （或经 PC817 光耦隔离）
                     │
5 V ──┬── 继电器线圈 ──┴── S8050 集电极
      │
   [1N4007 续流二极管，阴极朝 5 V]
      │
S8050 发射极 ── GND ； STM32 GND ── 5 V 电源 GND
```

- 续流二极管不可省：省掉后断电瞬间的反向尖峰可能击穿三极管并干扰 MCU

**代码侧**：有效电平必须用具名宏表达，不散落魔数（见 A.5）。

### A.4 备用引脚（本项目未用，可扩展）

| 用途 | 引脚 | 说明 |
| --- | --- | --- |
| 干净模拟输入 | PA5 / PA6 / PA7、PC0–PC5、PF3–PF7 | 插着 3.5″ 屏时**依然干净**（屏走 PD/PE/PG） |
| 干净数字 IO | PE0 / PE1 / PE6、PG1–PG5、PF12–PF15 | 板载未接任何器件 |
| 注意 | PC13 | 低驱动能力，不宜直接驱动继电器 |
| 注意 | PB8 / PB9 | 蜂鸣器 / 红外占用，不要再挂 I2C 从机 |

### A.5 执行器抽象与两阶段验证

**背景**：任务书要求"光线超过一定值 → **继电器**开启"以及"远程控制**继电器**动作"。继电器的**借用渠道已确认**，但到位需要时间。为了既不等待硬件、又不产生返工，采用**两阶段验证**：

| 阶段 | 执行器 | 目的 |
| --- | --- | --- |
| **阶段一 · 先验实验** | 板载 LED1（PE5）等效负载 | 把阈值、回差、手动优先、远程与语音控制等**控制逻辑**全部先行验证通过 |
| **阶段二 · 正式验证与验收** | 借用继电器模块（PD11） | 用**真实继电器**复测并完成答辩演示，任务书原文要求**无需任何替代说明即可完整达成** |

**关键约束**：两阶段之间必须**保留继电器接口空间**（见 A.6）；业务代码不得感知执行器换成了什么。

**做法**：执行器在固件里只是一个"GPIO + 有效电平"，把它抽象成一组宏，业务代码只调用 `Actuator_Set(on)`：

```c
/* control.h —— 执行器抽象：换硬件只改宏，业务代码零改动 */

/* ==== 阶段一配置：板载 LED1（PE5）作等效负载（先验实验）==== */
#define ACTUATOR_GPIO_PORT      GPIOE
#define ACTUATOR_GPIO_PIN       GPIO_PIN_5
#define ACTUATOR_CLK_ENABLE()   do { __HAL_RCC_GPIOE_CLK_ENABLE(); } while (0)
#define ACTUATOR_NAME           "LED1(等效负载)"

/* ==== 阶段二配置：借用继电器模块 ====
   到位后，把上面 4 行整体换成下面 4 行即可（其余代码不动）
#define ACTUATOR_GPIO_PORT      GPIOD
#define ACTUATOR_GPIO_PIN       GPIO_PIN_11
#define ACTUATOR_CLK_ENABLE()   do { __HAL_RCC_GPIOD_CLK_ENABLE(); } while (0)
#define ACTUATOR_NAME           "Relay(PD11)"
*/

#define ACTUATOR_ACTIVE_LEVEL   0      /* 0 = 低电平有效；LED1 与低电平触发模块一致，切换时无需修改 */

/* 唯一出口：业务代码只认这两个函数 */
void Actuator_Init(void);
void Actuator_Set(uint8_t on);
```

```c
/* control.c */
void Actuator_Init(void)
{
    GPIO_InitTypeDef gi = {0};
    ACTUATOR_CLK_ENABLE();
    HAL_GPIO_WritePin(ACTUATOR_GPIO_PORT, ACTUATOR_GPIO_PIN,
                      ACTUATOR_ACTIVE_LEVEL ? GPIO_PIN_RESET : GPIO_PIN_SET);  /* 先置断开 */
    gi.Pin   = ACTUATOR_GPIO_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ACTUATOR_GPIO_PORT, &gi);
}

void Actuator_Set(uint8_t on)
{
    GPIO_PinState s = on
        ? (ACTUATOR_ACTIVE_LEVEL ? GPIO_PIN_SET : GPIO_PIN_RESET)
        : (ACTUATOR_ACTIVE_LEVEL ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(ACTUATOR_GPIO_PORT, ACTUATOR_GPIO_PIN, s);
}
```

**当前配置（LED1 等效负载）与目标配置（继电器）的有效电平恰好一致，都是低电平**：

| 执行器 | 引脚 | 有效电平 | 说明 |
| --- | --- | --- | --- |
| 板载 LED1（当前） | PE5 | **低电平**点亮 | 官方 `led.c`：`LED1(1)` = 关闭 |
| 单路继电器模块（目标） | PD11 | **低电平**触发（推荐） | 见 A.3 的可靠性分析 |

因此**切换时连 `ACTUATOR_ACTIVE_LEVEL` 都不用改**，只需替换 `PORT` / `PIN` / `CLK_ENABLE` / `NAME` 四行。

**切换步骤（借用件到位后，约 5 分钟）**

1. 把 `ACTUATOR_GPIO_PORT/PIN/CLK_ENABLE/NAME` 改为 GPIOD / `GPIO_PIN_11` / `__HAL_RCC_GPIOD_CLK_ENABLE()` / `"Relay(PD11)"`；
2. 接模块三根线：VCC → 独立 5 V，GND → STM32 GND，IN → PD11；
3. 按 A.3 接线用跳线选**低电平触发**，然后按 T16 用例复测一遍阈值、回差、手动优先；
4. **不要**改动 `control.c` 的状态机、协议字段与 Qt 侧代码——它们只认 `relay` 这个逻辑名。

**演示与报告口径（分阶段，保证真实性）**

| 阶段 | 口径 |
| --- | --- |
| 先验实验（LED1） | 演示与报告**如实说明**"执行器当前为板载 LED 等效负载"，不要含糊称其为继电器 |
| 正式验证（借用继电器） | 用真实继电器完成 T02/T03/T05 与答辩演示，**无需任何替代说明** |

- 报告**硬件设计章节**按 A.3 写完整继电器方案（这是设计，应当完整）；
- 报告**系统测试章节**分两阶段记录：先验实验（LED 等效负载）+ 正式复测（真实继电器），并注明两阶段结论一致；
- 这样写既没有编造"用继电器测得的数据"，也完整体现了设计、验证与过程管理能力。

### A.6 保留继电器接口空间（明确要求）

借用件到位后要能"即插即用"，以下位置必须**预留、不得挪作他用**：

| 类别 | 预留内容 | 约束 |
| --- | --- | --- |
| 软件接口 | `ACTUATOR_*` 四行宏 + `Actuator_Init()` / `Actuator_Set()` | 业务代码只调函数，**不得直接出现 `GPIOE` / `GPIO_PIN_5` / `GPIOD` / `GPIO_PIN_11`** |
| 控制引脚 | **PD11** | 排针 / 焊盘位置不得接其他功能；已列入"不得挪用"清单 |
| 电源通路 | **独立 5 V** 与 **GND** 各一路引至继电器位置 | 与 STM32 共地；不得从板载 3.3 V 取电 |
| 物理空间 | 模块安放位置（成品单路模块约 50 × 25 mm）与固定方式 | 便于演示时观察触点动作，杜邦线留余量 |
| 驱动元件 | 裸继电器才需 S8050 / 1N4007 / 1 kΩ | 借用件为成品模块时**不占位、不必采购** |

**验收判据**：借用件到位后仅需替换 4 行宏 + 接 3 根线，**不修改任何业务代码、通信协议或上位机**（对应测试用例 T17）。

---

## 附录 B：关键常量与时间速查

| 项 | 值 | 依据 |
| --- | --- | --- |
| 光照 ADC | ADC3_IN6 / PF8 | `lsens.h` |
| ADCCLK | **12 MHz**（PCLK2 72 MHz ÷ 6） | `adc3.c`：`RCC_ADCPCLK2_DIV6`，官方注释"12M" |
| 采样时间 | 239.5 周期 | `ADC_SAMPLETIME_239CYCLES_5` |
| 单次转换 | 252 周期 = **21 µs** | 239.5 + 12.5；官方注释亦为 21 µs |
| 8 次自写均值 | **≈ 0.17 ms** | 8 × 21 µs |
| 官方 `adc3_get_result_average` | 每样本 `delay_ms(5)`，8 次 **≈ 40 ms**（阻塞） | `adc3.c` |
| 采集 / 上报周期 | 1 s | 设计 |
| 默认阈值 / 回差 | `th = 2000`、`hys = 150`（light 单位） | 设计 |
| 蜂鸣器 | PB8，高电平响 | 官方 `beep.c` |
| 执行器 | 阶段一 **PE5（LED1 等效负载）** / 阶段二 **PD11（借用继电器，低电平触发）** | A.5 |
| 运行心跳 | LED0（PB5），**每 5 s 翻转一次** | 设计 |
| EEPROM | AT24C02 @ I2C1 PB6/PB7，8 字节（1 页），写周期 ≤ 5 ms | 芯片手册 |
| IWDG 溢出 | **≈ 2.0 s**（40 kHz LSB ÷ 64 = 625 Hz，重装 1250） | 计算 |
| Qt 监听 / 推理端口 | **9092** / **8092** | 设计 |
| 静默失效判定 | 3 s（3 × 上报周期） | 设计 |
| 曲线保留点数 | 300 | 设计 |

**实测记录（2026-10-12，真实工具链构建）**

| 项 | 实测值 | 说明 |
| --- | --- | --- |
| 固件编译器 | Keil MDK 5.36 / ARMCLANG V6.24 | `UV4 -b greenhouse.uvprojx` |
| 固件构建结果 | **0 Error(s), 0 Warning(s)** | Code=57260 / RO-data=13360 / RW-data=32 / ZI-data=3136 |
| **Flash 占用** | **70652 B（69.0 KB）= 512 KB 的 13.5%** | Code + RO + RW |
| **RAM 占用** | **3168 B（3.1 KB）= 64 KB 的 4.8%** | RW + ZI |
| hex 大小 | 198788 B | `firmware/Output/greenhouse.hex` |
| 上位机工具链 | Qt 6.12.0 + MinGW gcc 13.1.0 + CMake 3.30.5 + Ninja | TTS 模块未装 → 自动关闭并降级 |
| 上位机运行验证 | 监听 `0.0.0.0:9092`，虚拟节点接入后长连接 ESTABLISHED | 与"Qt 监听局域网网卡"的设计一致 |
| 契约测试 | 11/11 通过 | `py tools/contract_test.py` |

> **构建路径陷阱（必读）**：源码路径含中文时 Qt 的 AutoMoc 无法写出生成文件
> （`moc: Cannot create ... No such file or directory`），且**8.3 短路径在本机仍是中文**，
> 不能用作绕过手段。`tools/build.ps1` 会自动建立纯 ASCII 目录联接后构建。
> 详见 [AGENTS.md](AGENTS.md) 已知陷阱第 8 条。

---

## 修订记录

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| **V1.1** | 2026-10-12 | 固件与上位机实现**真实构建**（Keil ARMCLANG 0 错误 0 警告；Qt 6.12+MinGW 构建通过并运行验证监听 0.0.0.0:9092）；补入 HAL/CMSIS/BSP 与 Keil 工程由脚本生成；记录实测 Flash 69.0 KB(13.5%)/RAM 3.1 KB(4.8%)；新增 tools/build.ps1 一键构建（含中文路径规避）与 bump_version.py；修复虚拟节点把 recv 超时误判为断链导致的重连风暴 |
| **v1.0** | 2026-10-12 | **首版基线**。硬件事实逐项核对开发板官方例程与 IO 分配表：光敏 **PF8/ADC3_IN6**（官方 readme 误写 PA1，已注明）、执行器目标 **PD11**（PD5 属 FSMC 不得挪用）、蜂鸣器 PB8 高电平响、LED 低电平点亮。引入 `light = 4095 - adc` 统一阈值语义、手动优先（`mode` + `auto`）、IWDG、透传行缓冲、链路保活与离线判定、EEPROM 魔数与不变式校验。执行器采用**两阶段验证**（阶段一 LED1 先验实验 / 阶段二借用继电器正式验证与验收），附录 A.6 规定**保留继电器接口空间**。附录 A 为引脚、电平极性与阈值语义的唯一事实来源；docx 由 `tools/md2docx.py` 从 md 生成 |
