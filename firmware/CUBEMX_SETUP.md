# firmware 集成与上板手册（CUBEMX_SETUP）

> 本手册把"业务代码已写好"变成"能在精英版 V2 上编译烧录"。
> 核心归属模型一句话：**业务代码已自带全部外设初始化，CubeMX 只负责生成时钟树 + HAL 脚手架 + 启动文件 + 空 MSP，绝不替业务代码去配任何外设/GPIO 引脚。**

---

## 0. 先跑自检（生成工程后、编译前）

```bash
py firmware/prebuild_check.py
```

它会列出"缺文件 / 重复定义 / 会编译不过"的项。下面第 5 节是它的判据来源。

---

## 1. 集成归属模型（必读，避免双初始化/抢引脚）

本仓库业务代码（`Core/Src` 下各模块）**已经把初始化写完**，CubeMX 不要再配：

| 资源 | 业务代码里的归属 | CubeMX 应如何处理 |
| --- | --- | --- |
| 时钟树 72 MHz / ADCCLK 12 MHz | `main.c::SystemClock_Config()` | CubeMX **配置时钟树**后由生成逻辑产出（或直接用本仓库 `main.c`，已手写） |
| `HAL_Init` / `SysTick` | HAL 库 + `stm32f1xx_it.c::SysTick_Handler` | CubeMX 生成脚手架，**保留 SysTick_Handler** |
| PF8 = ADC3_IN6 | `adc_light.c::MX_ADC3_Init()`（含 GPIOF 时钟+模拟输入） | **不要**在 CubeMX 启用 ADC3 |
| PB6/PB7 = I2C1 | `eeprom.c::MX_I2C1_Init()`（含 GPIOB 时钟+AF_OD） | **不要**在 CubeMX 启用 I2C1 |
| PB10/PB11 = USART3 | `esp8266.c::MX_EspUart_Init()`（含 GPIOB 时钟+NVIC） | **不要**在 CubeMX 启用 USART3 |
| PB8 蜂鸣器 / PE5 执行器 / PB5 心跳 | `control.c::Buzzer_Init/Actuator_Init` + `main.c::Led0_Init` | **不要**在 CubeMX 配这些 GPIO |
| IWDG **≈6.4 s**（重装值 4000） | `main.c::MX_IWDG_Init()` | **不要**在 CubeMX 启用 IWDG |
| LCD（FSMC） | 官方 BSP `lcd.c`（或桩） | 仅在引入 BSP 时由 BSP 自理 |

**为什么这样分**：业务模块的 `MX_*_Init()` 调 `HAL_*_Init()` 时会回调弱 `HAL_*_MspInit`。若 CubeMX 也启用了这些外设，生成的 MSP 会再去配同一组 GPIO → 双初始化、引脚被抢、FSMC 时序被破坏（引脚主权与执行器归属见 `docs/lessons.md#actuator`）。让 CubeMX **不启用**这些外设，MSP 就是空桩，业务代码成为唯一 owner。

---

## 2. CubeMX `.ioc` 点击配置（按此设，别多配）

1. **MCU**：选 `STM32F103ZET6`（LQFP144，高密度 512 KB）。
2. **Pinout & Configuration → System Core → SYS**：Debug = `Serial Wire`（占用 PA13/PA14，即 SWD，烧录用）。**其余引脚一律不要在这里分配**。
3. **Pinout → RCC**：`High Speed Clock (HSE)` = `Crystal/Ceramic Resonator`（板载 8 MHz 晶振）。
4. **Clock Configuration**（关键，错则 ADC 采样时间全错）：
   - 输入 HSE = 8 MHz
   - PLL Source = HSE， `PLL Mul` = ×9 → **SYSCLK = 72 MHz**
   - `HCLK` = 72 MHz（AHB ÷1）
   - `PCLK1` = 36 MHz（APB1 ÷2）
   - `PCLK2` = 72 MHz（APB2 ÷1）
   - **`ADCCLK` = PCLK2 ÷6 = 12 MHz**（`RCC_ADCPCLK2_DIV6`）
   - 完成后右侧应显示：SYSCLK 72 / HCLK 72 / PCLK1 36 / PCLK2 72 / ADCCLK 12。
5. **不要触碰**：Analog→ADC3、Connectivity→I2C1/USART3、Timers→IWDG、任何 GPIO 引脚（除 SWD）。
6. **Project Manager → Code Generator**：
   - ☐ `Generate peripheral initialization as a pair of .c/.h files per peripheral` —— **取消勾选**（不生成多余 MX 文件）
   - ☑ `Keep User Code when re-generating`
   - ☑ `Delete previously generated files when not re-generated`
   - Toolchain/IDE：Keil 选 `MDK-ARM`；CLion 选 `STM32CubeIDE`/CMake 均可（本仓库用 CMake 也行）
7. **Project Manager → Advanced Settings**：确认 `STM32F1xx HAL` 被勾选（生成 `stm32f1xx_hal.c` 等）。

> 设备宏 `STM32F103xE` 由 CubeMX 自动加进工程（`#define` 或工具链 `-D`），它是 HAL 编译硬性前提；若用 CMake 需自行在编译定义里加 `STM32F103xE`。

---

## 3. 生成后必须做的文件收尾（否则链接/编译失败）

> **现状说明（骨架已在仓库内，不跑 CubeMX 也能直接编译 —— 已实测 0 Error / 0 Warning）**
>
> 本节列的是"从零跑一遍 CubeMX"时的收尾动作。**本仓库已经把骨架备齐了**：
> - `Drivers/`（163 个文件）＝ HAL + CMSIS + 启动文件 + 官方 BSP（`Drivers/BSP/LCD/lcd.c/.h` **含字库**）
> - `Middlewares/cJSON/` 已就位
> - `Core/Inc/stm32f1xx_hal_conf.h`、`Core/Src/stm32f1xx_it.c`、`Drivers/CMSIS/Device/.../system_stm32f1xx.c` 已就位
> - Keil 工程 `Projects/MDK-ARM/greenhouse.uvprojx` 由 **`py tools/make_keil_project.py`** 生成
>   （**增删源文件后必跑**；官方模板缺 `stm32f1xx_hal_i2c.c`（AT24C02）与 `stm32f1xx_hal_iwdg.c`（看门狗），脚本已补入）
> - 一键验证：`powershell -ExecutionPolicy Bypass -File tools\build.ps1`
>
> 因此**你现在只需要补一件事**：把 `Core/Inc/wifi_config.h.example` 拷成 `wifi_config.h` 并填真实值（见步骤 ④）。
> 只有当你**重新用 CubeMX 生成工程**时，才需要按下面 ①～⑥ 逐步收尾。

生成到 `firmware/`（与 `Core/Src|Inc` 同层，HAL/CMSIS 落 `Drivers/`）之后：

| 步骤 | 操作 | 原因 |
| --- | --- | --- |
| ① | **用本仓库 `Core/Src/main.c` 与 `Core/Inc/main.h` 覆盖** CubeMX 生成的同名文件 | 仓库 `main.c` 自带时钟树 + IWDG + 主循环 + 全局状态；用生成的会缺业务逻辑甚至有两个 `main()` |
| ② | 打开 `Core/Src/stm32f1xx_it.c`，**删除其中的 `USART3_IRQHandler`**（保留 `SysTick_Handler`） | `USART3_IRQHandler` 已在 `esp8266.c` 定义（经 `ESP_IRQHandler` 宏，`ESP_UART = USART3`），重复会 `multiple definition`。**注**：本仓库现用的 `it.c` 取自官方例程，本身就没有 `USART3_IRQHandler`，无需删；只有 CubeMX 新生成的才要检查 |
| ③ | 打开 `Core/Src/stm32f1xx_hal_msp.c`，确认 `HAL_ADC_MspInit` / `HAL_I2C_MspInit` / `HAL_UART_MspInit` **为空**（因为第 2 步没启用这些外设，本就空；若被填了引脚代码则清空） | 与业务 `MX_*_Init` 双初始化 |
| ④ | 拷贝 `Core/Inc/wifi_config.h.example` → `Core/Inc/wifi_config.h`，填真实 SSID / 密码 / 上位机 PC 的局域网 IP | `wifi_config.h` 不入仓；缺失时 `main.c` 用占位值，能编译但连不上网 |
| ⑤ | `Middlewares/cJSON/` **已随仓库提供，无需另行下载**。把 `cJSON.c` 加入编译、`cJSON.h` 加入 include 路径；**不要**把 `cjson_test.c` 加入工程（它是 PC 端单元测试，含 `main()`，会与固件 `main()` 冲突）。**`startup_stm32f103xe.s` 的 `Heap_Size` 必须是 `0x1000`**（本仓库实际值；早期文档写的"`0x800` 以上"已过时）：cJSON 是 **`malloc` 型**解析器，每个字符串 / 每个数字 / 每个节点都要分配，一条典型指令约 **300~390 B**，而 `0x200`（512 B）只够 **60~77%** —— 堆不足时 `malloc` 失败 → `cJSON_Parse` 返回 NULL → **整条指令被静默丢弃**。堆预算的实测依据见 `docs/lessons.md#code-invariants` | 协议层 `protocol.c` 用 cJSON 解析 |
| ⑥ | **LCD：已随仓库提供**（`Drivers/BSP/LCD/lcd.c/.h`，含字库），默认 `LCD_SHOW_ENABLED=1` 可直接编译。若你**不打算接屏**，把 `Core/Inc/main.h` 的 `LCD_SHOW_ENABLED` 改为 `0` 即可编译为桩，采集/控制/上报不受影响 | 默认 `LCD_SHOW_ENABLED=1`，没 BSP 会 `lcd.h: No such file`。`lcd_show.c` 按 V2 BSP 的 `lcd_show_string(x,y,w,h,size,char*,color)` 接口编写，`prebuild_check.py` 会校验 API 形态 |

---

## 4. 加入业务源码与包含路径

把本仓库这些文件加入工程（Keil 加组 / CMake 加 `target_sources`）。
**Keil 工程不要手改**：跑 `py tools/make_keil_project.py` 重新生成 `greenhouse.uvprojx`，脚本会按下面的清单填好分组、包含路径与宏定义。

```
Core/Inc : main.h adc_light.h control.h protocol.h eeprom.h esp8266.h lcd_show.h \
           wifi_config.h（由 .example 生成）stm32f1xx_hal_conf.h stm32f1xx_it.h
Core/Src : main.c adc_light.c control.c protocol.c eeprom.c esp8266.c lcd_show.c stm32f1xx_it.c
Middlewares/cJSON : cJSON.c cJSON.h       ← cjson_test.c 仅 PC 端测试，含 main()，勿加入固件工程
Drivers/BSP/LCD   : lcd.c（字库随 lcd.h）  ← 已在仓库内
Drivers/CMSIS/Device/ST/STM32F1xx/Source/Templates/system_stm32f1xx.c
```

包含路径（`-I`）：`Core/Inc`、`Middlewares`、`Middlewares/cJSON`、`Drivers`、
`Drivers/BSP/LCD`、`Drivers/STM32F1xx_HAL_Driver/Inc`、
`Drivers/CMSIS/Device/ST/STM32F1xx/Include`、`Drivers/CMSIS/Include`。

> `Drivers` 与 `Drivers/BSP/LCD` 两条都要留：BSP/SYSTEM 的头文件用 `"./BSP/LCD/lcd.h"` 这类相对
> `Drivers` 的写法，而 `lcd_show.c` 用的是 `#include "lcd.h"`。

编译定义（`-D`）：`STM32F103xE`、`USE_HAL_DRIVER`、以及 `LCD_SHOW_ENABLED=0`（若走不接屏的方案）。

**HAL 模块清单（易漏）**：本项目用到 ADC / GPIO / I2C / IWDG / RCC / UART / SRAM(FSMC) / DMA / PWR / CORTEX。
官方 `实验20` 模板**只有 ADC/GPIO/UART/SRAM 等**，**缺 `stm32f1xx_hal_i2c.c`（AT24C02）与 `stm32f1xx_hal_iwdg.c`（看门狗）**——
手工建工程极易漏，漏了会报 `HAL_I2C_Init` / `HAL_IWDG_Init` 未定义。`make_keil_project.py` 已把它们列入。

---

## 5. 编译前自检判据（与 `prebuild_check.py` 一一对应）

- `Drivers/` 存在且含 `stm32f1xx_hal.c` / `system_stm32f1xx.c`；启动文件 `startup_stm32f103xe.s` 入工程。
- HAL 已包含 **I2C 与 IWDG** 模块（`stm32f1xx_hal_i2c.c` / `stm32f1xx_hal_iwdg.c`），且 `stm32f1xx_hal_conf.h` 里 `HAL_I2C_MODULE_ENABLED` / `HAL_IWDG_MODULE_ENABLED` 为打开状态。
- `stm32f1xx_it.c` 无 `USART3_IRQHandler` 重复定义，且保留 `SysTick_Handler`。
- `stm32f1xx_hal_msp.c` 的 MSP 函数未配置业务主权引脚（PF8/PB6-7/PB10-11/PB8/PE5/PB5）。
- `Middlewares/cJSON/cJSON.c` 与 `cJSON.h` 存在并已加入编译（`cjson_test.c` 仅 PC 端验证用，勿加入固件编译）。
- `lcd.h` 存在 **或** `LCD_SHOW_ENABLED=0`。
- `wifi_config.h` 已创建（或至少存在 `.example`）。
- `Core/Src/main.c` 是本仓库业务版（含 `MX_IWDG_Init`），非 CubeMX 生成版。
- 工具链定义 `STM32F103xE`。

---

## 6. 上板验证清单（对应里程碑，按 Tech_Design）

| 用例 | 操作 | 期望 |
| --- | --- | --- |
| M1-a | 手电照射 / 遮挡 LS1 | 光强上升 / 下降，方向正确（`light = 4095 - adc`） |
| M1-b | 万用表测 PF8 电压 | 光照越强电压越低 |
| M2-a | 上电瞬间 | 蜂鸣器**不响**、执行器（LED1）**灭** |
| M2-b | 光强越过阈值 / 回差以下 | LED1 亮 / 灭，蜂鸣器同步；LCD 状态一致 |
| M2-c | 连续运行观察 PB5 | LED0 每 5 s 翻转一次（心跳） |
| M2-d | 设一次阈值后断电重启 | 阈值保留（EEPROM 掉电记忆） |
| M2-e | 拔 EEPROM / 写坏数据后上电 | 回退默认 `th=2000 / hys=150`，正常启动 |
| M3-a | ESP8266 上电 | 自动连 WiFi → 建 TCP → 透传；Qt 收到 `report` |
| M3-b | 拔 ESP8266 供电 | 本地闭环（采集/显示/报警）不受影响 |
| M2b | 继电器到位后 | 只换 `control.c` 里 `ACTUATOR_*` 4 行宏 + 接 3 根线，复测 M2-a~c 全部通过 |

> 阶段一用 LED1（PE5）等效执行器；阶段二继电器到位，仅改 `control.c` 的 4 行 `ACTUATOR_*` 宏（`GPIOE/PIN_5` → `GPIOD/PIN_11`），业务代码其余不动。注意 PD11 排针/焊盘不得接其他功能（引脚主权与执行器归属见 `docs/lessons.md#actuator`）。

---

## 7. 常见编译问题速查

- `USART3_IRQHandler` 重复定义 → 删 `stm32f1xx_it.c` 里的同名函数（见步骤②）。
- `lcd.h: No such file` → 走方案 B，把 `LCD_SHOW_ENABLED` 置 0（见步骤⑥）。
- `cJSON.h: No such file` → 步骤⑤ 未加 cJSON 或 include 路径缺失。
- cJSON 解析异常（字段读不出 / 帧被拒）→ 上板前先在 PC 跑 `gcc cJSON.c cjson_test.c -o t && ./t` 验证子集解析正确（退出码 0 即通过）；本仓库已内置与官方兼容的子集，如需完整功能整体替换为官方 cJSON 即可。
- `unknown type name 'ADC_HandleTypeDef'` / `STM32F103xE` 未定义 → 编译定义漏了 `STM32F103xE` + `USE_HAL_DRIVER`。
- `ADC_REGULAR_RANK_1` 不存在 → HAL 版本较旧，删 `adc_light.c` 里 `ch.Rank` 赋值（保留 Rank 默认 1）。
- 烧录后不运行 → 启动文件选错（必须 `startup_stm32f103xe.s`）、或 SWD 被关（保持 SYS=Serial Wire）。
- 上电蜂鸣器一直叫 → 确认 `Buzzer_Init()` 先于业务逻辑、且 `Buzzer_Set(0)` 为关（高电平响）；上电已置 `Control_ApplyOutput(0)`。
