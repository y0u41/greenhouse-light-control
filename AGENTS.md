# 智能温室感光控制系统 AGENTS（入口文件）

> 本文件是**入口**：只讲**规则红线**、**文档地图**与"动手前必读"。
> **命令一律不在此重复** —— 构建、运行、脚本用法全部见 `README.md`。
> 文档集版本：**V1.1**（2026-10-12）

## 一、文档地图（要查什么去哪）

| 要查什么 | 去哪 |
| --- | --- |
| 需求范围 / 验收标准 | `PRD.md` |
| 引脚分配 / 电平极性 / ADC 与阈值语义（**唯一事实来源**） | `Tech_Design.md` 附录 A |
| 协议帧与字段 / 阈值存储布局 / 端口地址 / 看门狗参数 | `Tech_Design.md` 对应章节 + 附录 B |
| 方案论证 / 硬件选型 / 开发计划 / 测试方案（对外提交稿） | `项目规划书.md`（`.docx` 由 `tools/md2docx.py` 生成，**勿手改**） |
| 设计报告（任务书第 4 项交付物） | `设计报告.md`：**实测值与【待测】严格区分，禁止把推测值写成实测结果** |
| **命令 / 构建 / 上手步骤 / 目录结构 / 里程碑** | `README.md` §快速开始、§目录结构 |
| CubeMX 与外设配置（含"引脚主权"归属表） | `firmware/CUBEMX_SETUP.md` |
| **踩坑全记录（改代码前务必读）** | **`docs/lessons.md`**（原「已知陷阱」全表已并入，含编号对照） |
| 跨层一致性守卫（改任一处后必跑） | `tools/consistency_check.py`（30 项） |

- 冲突处理：**PRD > Tech_Design > 项目规划书 > 代码现状**；发现冲突先向用户提出，不要自行默默改掉
- 涉及引脚 / 协议 / 阈值的改动，**先改 `Tech_Design.md` 附录 A**，再同步其余文档与代码
- 文档现阶段置于仓库根目录；若后续迁入 `docs/`，必须同步更新本文件、`Tech_Design.md` 目录树与所有交叉引用

## 二、动手前必读（最危险的 5 条）

1. **半主机必须显式关闭**：不关则库初始化执行 `bkpt`，无调试器即 HardFault，**在 `main()` 之前就死**
   （表现为"上电毫无反应、串口 0 字节"）→ `docs/lessons.md#semihosting`
2. **IWDG 三件事**：重装值 ≤ 4095（RLR 只有 12 位）；一旦启动**系统复位不清它**（改参数必须整板断电验证）；
   `MX_IWDG_Init()` **只能紧邻 `for(;;)` 之前** → `docs/lessons.md#iwdg`
3. **先怀疑测量工具，再怀疑被测系统**：ST-Link `mode=hotplug` 会复位目标，读到的常是"复位后刚开始启动"
   的瞬间；读数脚本一律用 Python 写，不要 pwsh 内联（`r`/`rd`/`h` 是内置别名）→ `docs/lessons.md#debug-only-once`
4. **引脚主权**：执行器 PD11，**绝不能放 PD5**（FSMC_NWE）；FSMC 占用脚一律不得挪用；业务代码不得直写引脚
   → `docs/lessons.md#actuator` + `Tech_Design.md` 附录 A
5. **光敏是 PF8 = ADC3_IN6，不是 PA1**（官方 readme 笔误）；`light = 4095 - adc`，**阈值判断只用 `light`**；
   不要复用 `lsens_get_val()` → `docs/lessons.md#light-sensor`

> **完整踩坑清单见 [`docs/lessons.md`](docs/lessons.md)（改代码前务必读）** ——
> 上面 5 条只是一句话速查；现象、证据、根因、修法、复现命令都在那里。

## 三、安全红线

- **llama-server 绑 `127.0.0.1`**（只服务本机 Qt）。**Qt NodeServer 必须监听局域网网卡**：默认
  `tcp/bindAddress = 0.0.0.0`（或 PC 的 LAN IP）—— 节点是局域网上的**独立设备**，Qt 只绑回环会导致永远连不上；
  用防火墙把 9092 入站限制在内网
- **端口不可混用**：llama-server = 8092，Qt NodeServer = 9092
- `llm/token` 走配置项；llama-server 启动命令未启用 `--api-key`，为空时**不要发送** `Authorization` 头，
  也不要在源码里硬编码 Token
- `set_th` 必须校验：范围 0–4095 **且 `value > hys`**，非法值拒绝并提示，**不静默截断**
- 大模型返回的 JSON **必须经 `CommandEngine` 校验后再下发**，不直接透传给 STM32
- **ESP8266 的 WiFi SSID/密码不入仓**（不提交的 `wifi_config.h` + 提交 `wifi_config.h.example`）
- 执行器有效电平必须用具名宏 `ACTUATOR_ACTIVE_LEVEL` 表达，不散落魔数；裸继电器必须用三极管/光耦 +
  续流二极管，**成品模块已内置驱动元件，无需外接**
- **启用 IWDG**；ESP8266 AT 等待改为带喂狗的有限轮询等待，**禁止无喂狗的阻塞延时**
- 硬件引脚分配以 **`Tech_Design.md` 附录 A** 为准；改引脚必须同步附录 A 与 `项目规划书.md`

## 四、禁止事项

- 不要提交 WiFi 密码、API Token、模型文件（`.gguf`）、构建产物或历史数据
- 不要修改 `项目内容.txt`（任务书原文，只读）；不要手工改生成的 `.docx`
- 不要把业务逻辑写进 UI 信号槽，也不要在固件中断里做耗时操作
- 不要在主线程调用 llama.cpp API（会冻结界面），必须放工作线程
- 不要绕过 `CommandEngine` 直接把大模型输出发给 STM32
- **不要把执行器或任何外设配到 FSMC 占用脚**（PD0/PD1/PD4/PD5/PD8~PD10/PD14/PD15、PE7~PE15、PG0/PG12、PB0）
- **不要把执行器的端口/引脚/有效电平写死在业务逻辑里** —— 必须走 `ACTUATOR_*` 宏
- **不要直接复用 `lsens_get_val()` 的 0–100 返回值**充当协议里的 `adc`
- 不要对 `main` 强推（force push）、改写已推送历史，或删除/改写他人未合并的分支；不要 rebase / force push 掉基线提交
- 不要擅自引入重量级依赖（新的 GUI 框架、网络库、JSON 库），先说明理由并等确认

## 五、编码规范要点

- **固件（C）**：遵循 HAL 命名（`HAL_XXX_XXX` / `Module_Function`）；中断只置标志位，业务逻辑放主循环；
  GPIO 用 `HAL_GPIO_WritePin`，不直接操作寄存器（有性能理由须注释）；主循环与 AT 等待路径**禁止长 `HAL_Delay`**；
  头文件保护用 `#ifndef` 或 `#pragma once`
- **上位机（C++/Qt）**：类名 PascalCase、成员 `m_` 前缀、信号 `signalXXX`、槽 `onXXX`；禁止主线程阻塞
  （HTTP / 大文件 IO 放工作线程）；网络用异步信号槽，不手写 `while` 等；UI 只在主线程更新；
  一个文件一个主要类，超 500 行考虑拆分
- **通用**：注释用中文，解释「为什么」而非复述代码在做什么；提交前通过编译与基本测试，不留 `qDebug()` 与注释掉的死代码

## 六、协议与存储：必须守住的不变式

帧格式、字段表、示例帧、EEPROM 8 字节布局等细节见 **`Tech_Design.md`**；此处只列**不许违反**的部分。

- `light = 4095 - adc`（0 = 全暗，4095 = 最强）；`adc` 为原始 12 位码值；**阈值判断只用 `light`**
- 所有帧必带 `node`；上行 `report`/`ack`/`evt`；下行只有
  `relay_on`/`relay_off`/`auto`/`set_th`/`get_state`/`alarm_test` —— **不存在 `query` 这个动作名**
- `relay_on`/`relay_off` **切手动模式**（手动优先，不得被自动逻辑回弹）；`auto` 切回自动并按当前 `light` 重算；
  `alarm_test` 只脉冲蜂鸣器（**不改模式、不动执行器**）
- **不变式 `0 <= hys < th <= 4095` 必须在两处强制**（EEPROM 加载、`set_th` 校验）；违反会让 `th - hys`
  无符号下溢、比较永假、**继电器锁死无法释放**
- EEPROM 魔数 / 校验和失败、不变式不满足 → **一律回退默认值，不阻塞启动**；新增字段必须给默认值或允许回退

## 七、测试与完成定义（DoD）

- 主机原生测试在 `tests/host`（`control.c` / `protocol.c` 的行为测试，**不覆盖 `main.c`**），
  **必须全绿**；命令见 `README.md` §快速开始，已接入 `build.ps1 -Target test`
- 硬件相关改动必须在真实开发板或 `tools/simulate_node.py` 虚拟节点上验证过
- ADC 方向（照射 → `light` 上升、`adc` 下降）、回差三点边界、手动优先不回弹、上电执行器断开且蜂鸣器不响、
  `ACTUATOR_*` 宏替换后 `T02/T03/T05` 复测通过
- **断网本地闭环**：拔掉 ESP8266 或断 WiFi 后，采集 / 显示 / 报警必须照常工作
- 协议鲁棒性：合法 / 非法 JSON、缺字段、多余字段、超长无 `\n` 数据均不崩溃
- 修 bug 时**先补一个能复现的测试或调试步骤，再改代码**
- 涉及需求 / 协议 / 引脚 / 阈值的变更须同步 `PRD.md`、`Tech_Design.md`、`项目规划书.md` 并重跑 `tools/md2docx.py`

## 八、提交与协作

- 分支：`main`（保护分支）、`feat/<主题>`、`fix/<主题>`；Conventional Commits（feat / fix / docs / refactor / test / chore）；
  一个提交只做一件事，**固件与 Qt 分开提交**；PR 描述写清"改了什么、为什么、怎么验证、影响哪些文档"
- 基线提交 `9defdeb`（327 文件全量入仓）与 `06aa657`（删除 6.8 MB 历史遗留副本）**不要 rebase / force push 掉** —— `_attic/` 靠它们才可恢复
- **★ 交付 / 交作业必须用 `tools/package_submission.py`（内部走 `git archive`），不要右键压缩目录 ★**：
  `.gitignore` 保护不了"整目录打 zip 发群"，`wifi_config.h` 的真实 WiFi 密码会随包带走；命令与自校验项见 `README.md` §交付打包
- 提交前自检：`git status --porcelain` 应为空；确认 `wifi_config.h` 被 `.gitignore` 命中；
  `git log --all --pretty=format: --name-only | grep wifi_config` 应只有 `.example`
- 命令与脚本用法见 `README.md` §快速开始；改任一处后必跑 `tools/consistency_check.py`

## 九、修订记录

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| **V1.1** | 2026-10-12 | 固件与上位机实现**真实构建**（Keil ARMCLANG 0 错误 0 警告；Qt 6.12+MinGW 构建通过并运行验证监听 0.0.0.0:9092）；补入 HAL/CMSIS/BSP 与 Keil 工程由脚本生成；记录实测 Flash 69.0 KB(13.5%)/RAM 3.1 KB(4.8%)；新增 tools/build.ps1 一键构建（含中文路径规避）与 bump_version.py；修复虚拟节点把 recv 超时误判为断链导致的重连风暴 |
| **V1.0** | 2026-10-12 | **首版基线**（与文档集 V1.0 对齐）。含"已知陷阱"7 条（PF8/ADC3、光照方向、PD11 与接口预留、端口错开、PB8 高电平响、TTS 模块与语音名、LED 分工）；协议含 `auto` / `light` / `hys` / `mode`；EEPROM 8 字节含魔数；IWDG 与断网本地闭环要求；`项目规划书.docx` 由 md 生成 |
