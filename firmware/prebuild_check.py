# firmware/prebuild_check.py
# CubeMX 工程骨架落定后的静态自检：在编译前跑一遍，提前暴露"缺文件 / 会冲突 / 会编译不过"。
#
# 用法：py firmware/prebuild_check.py
#
# 设计：本脚本只做"文件存在性 + 文本冲突"检查，不依赖编译器；
#       没生成 CubeMX 工程也能跑（会把缺失项列出来，作为你的取数清单）。

import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))

# 业务代码已自带初始化的资源（CubeMX 绝不能再去配置，否则双初始化/抢引脚）
BUSINESS_OWNED_PINS = {
    "PF8": "ADC3_IN6 光照采集",
    "PB6/PB7": "I2C1 EEPROM",
    "PB10/PB11": "USART3 ESP8266（ATK 模块接口）",
    "PB8": "蜂鸣器(高电平响)",
    "PE5": "执行器阶段一(LED1)",
    "PB5": "LED0 心跳",
}
DEVICE_MACRO = "STM32F103xE"   # ZET6 = 高密度 512KB


def _find(pattern_dir, names):
    """在 pattern_dir 子树里找任一文件名，返回首个命中路径或 None。"""
    hits = {}
    for base, _, files in os.walk(pattern_dir):
        for f in files:
            low = f.lower()
            if low in names:
                hits.setdefault(low, os.path.join(base, f))
    return hits


def check_generated_scaffold(errors, warns):
    drivers = os.path.join(ROOT, "Drivers")
    core = os.path.join(ROOT, "Core")
    if not os.path.isdir(drivers):
        errors.append("Drivers/ 不存在：CubeMX 尚未生成 HAL 与 CMSIS（含 stm32f1xx_hal.c / 启动文件 / system_stm32f1xx.c）")
        return
    d = _find(drivers, {"stm32f1xx_hal.c"})
    if "stm32f1xx_hal.c" not in d:
        errors.append("缺少 stm32f1xx_hal.c（HAL 核心）")
    # ★ 修死守卫：system_stm32f1xx.c 与启动文件**都不在 Drivers/ 下**：
    #   · system_stm32f1xx.c    -> Core/Src/（业务工程维护）
    #   · startup_stm32f103xe.s -> Projects/MDK-ARM/（Keil 工程目录）
    # 旧版在 Drivers/ 子树里找这两个 -> **永远找不到** -> 必然 append 一条
    # "缺少 system_stm32f1xx.c" 并以退出码 1 结束。这是"守卫看着在守、其实
    # 守不住"的典型：它报的错是假的，真正的缺失反而检不出来。
    if not os.path.isfile(os.path.join(core, "Src", "system_stm32f1xx.c")):
        errors.append("缺少 Core/Src/system_stm32f1xx.c（系统时钟/总线默认配置）")
    # 启动文件（高密度 ZET6）
    startup = {}
    for _r in (os.path.join(ROOT, "Projects"), drivers):
        if os.path.isdir(_r):
            _f = _find(_r, {"startup_stm32f103xe.s", "startup_stm32f103xe.sfd",
                            "startup_stm32f103xg.s"})
            if _f:
                startup = _f
                break
    if not startup:
        warns.append("未找到 startup_stm32f103xe.s（ZET6 高密度启动文件）；Keil/CLion 工程需包含它")


def _strip_comments(txt):
    """去掉 C 注释，只留下真正的代码。

    为什么要去注释：本仓库的 esp8266.c 头注释里**故意**写了替代方案
    `#define ESP_IRQHandler  USART2_IRQHandler`（那是可选的引脚配置说明），
    而 it.c 的官方例程里也有一行注释掉的 `/*void PPP_IRQHandler(void) ...*/`。
    若直接全文搜索函数名，会把注释里的字样误判成"重复定义"。
    """
    txt = re.sub(r"/\*.*?\*/", " ", txt, flags=re.S)   # /* 块注释 */
    txt = re.sub(r"//[^\n]*", " ", txt)                # // 行注释
    return txt


def check_msp_and_it(errors, warns):
    itc = os.path.join(ROOT, "Core", "Src", "stm32f1xx_it.c")
    if os.path.isfile(itc):
        with open(itc, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
        # 真正的冲突风险在 USART3：ESP8266 挂在 USART3（PB10/PB11），其
        # `void ESP_IRQHandler(void)` 就是 `USART3_IRQHandler`（见 esp8266.c 的宏块）。
        # 早期这里写的是 USART2 —— 那个函数本仓库根本不定义，条件**永远不可能命中**，
        # 是一条"看着在守卫、实际什么都没守"的假守卫。
        if "void USART3_IRQHandler" in _strip_comments(txt):
            errors.append("stm32f1xx_it.c 里重复定义了 USART3_IRQHandler；"
                          "必须删除该行（已在 esp8266.c 定义），否则链接报 multiple definition")
        if "void SysTick_Handler" not in txt:
            warns.append("stm32f1xx_it.c 缺少 SysTick_Handler（必须保留，内部 HAL_IncTick 给 HAL_GetTick 计时）")
    else:
        warns.append("stm32f1xx_it.c 尚未生成（CubeMX 生成后需删除其中的 USART3_IRQHandler）")

    msp = os.path.join(ROOT, "Core", "Src", "stm32f1xx_hal_msp.c")
    if os.path.isfile(msp):
        with open(msp, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
        # 我们的模块已在 MX_*_Init 里完成 GPIO，MSP 不应再配置这些引脚。
        # 注意 "PB6/PB7" 这类多引脚条目必须逐个比对：整体去斜杠后（PB6PB7）永远匹配不到。
        compact = txt.replace(" ", "")
        for pin, who in BUSINESS_OWNED_PINS.items():
            touched = [p for p in pin.split("/") if p in compact]
            if touched:
                warns.append("stm32f1xx_hal_msp.c 似乎触碰了 %s（%s）；"
                             "若 CubeMX 启用了对应外设请禁用，避免与业务 MX_*_Init 双初始化"
                             % ("/".join(touched), who))


def check_cjson(errors, warns):
    mid = os.path.join(ROOT, "Middlewares", "cJSON")
    if not os.path.isdir(mid):
        errors.append("Middlewares/cJSON 不存在：协议层用 cJSON 解析，需放入 cJSON.c/.h（只编译 cJSON.c）")
        return
    h = _find(mid, {"cjson.h", "cjson.h"})
    c = _find(mid, {"cjson.c"})
    if not h:
        errors.append("Middlewares/cJSON 下缺 cJSON.h")
    if not c:
        errors.append("Middlewares/cJSON 下缺 cJSON.c（需加入编译）")


def check_lcd(errors, warns):
    # 默认 LCD_SHOW_ENABLED=1，必须提供 lcd.h；否则编译前要把它置 0
    main_h = os.path.join(ROOT, "Core", "Inc", "main.h")
    enabled_default = True
    forced_zero = False
    if os.path.isfile(main_h):
        with open(main_h, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
        if "#define LCD_SHOW_ENABLED        0" in txt or "#define LCD_SHOW_ENABLED 0" in txt:
            forced_zero = True
        if "LCD_SHOW_ENABLED" not in txt:
            enabled_default = True
    lcd_h = _find(ROOT, {"lcd.h"})
    if forced_zero:
        return  # 已显式关显示，无 BSP 也能编
    if not lcd_h:
        errors.append("未找到 lcd.h 且 LCD_SHOW_ENABLED 未置 0：默认开显示会编译失败。"
                      "二选一：① 放入正点原子 BSP 的 lcd.h/.c(+字库)；② 把 main.h 的 LCD_SHOW_ENABLED 改为 0")
        return

    # API 形态守卫：lcd_show.c 按精英版 V2 资料盘（HAL BSP）编写，其接口是
    #   lcd_show_string(x, y, width, height, size, char *p, uint16_t color)
    # 早期寄存器版是 LCD_ShowString(...) / POINT_COLOR，签名不同，直接换 BSP 会编译失败。
    try:
        with open(list(lcd_h.values())[0], "r", encoding="utf-8", errors="ignore") as f:
            htxt = f.read()
    except OSError:
        return
    if "lcd_show_string" not in htxt:
        errors.append("找到的 lcd.h 未声明 lcd_show_string()：lcd_show.c 按 V2 HAL BSP 的 7 参带颜色接口编写。"
                      "若手上是早期寄存器版 BSP（LCD_ShowString / POINT_COLOR），"
                      "请换用 V2 HAL BSP，或把 main.h 的 LCD_SHOW_ENABLED 置 0")
    if "g_back_color" not in htxt:
        warns.append("lcd.h 未声明 g_back_color；lcd_show.c 会写该全局（字符串右侧填充色），编译可能报未定义")


def check_wifi_config(errors, warns):
    inc = os.path.join(ROOT, "Core", "Inc")
    if os.path.isfile(os.path.join(inc, "wifi_config.h")):
        return
    if os.path.isfile(os.path.join(inc, "wifi_config.h.example")):
        warns.append("wifi_config.h 尚未创建：请由 wifi_config.h.example 复制并填真实 SSID/密码/PC IP"
                     "（缺失时 main.c 用占位值，能编译但连不上网）")
    else:
        errors.append("wifi_config.h.example 也不存在，无法生成 wifi_config.h")


def check_duplicate_main(errors, warns):
    # 我们的 main.c 自带 SystemClock_Config 与 main()；CubeMX 生成版必须被覆盖，否则有两个 main
    main_c = os.path.join(ROOT, "Core", "Src", "main.c")
    if os.path.isfile(main_c):
        with open(main_c, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
        if "SystemClock_Config" in txt and "int main(void)" in txt and "MX_IWDG_Init" in txt:
            # 这是我们的业务 main.c（期望态）
            pass
        elif "int main(void)" in txt and "MX_IWDG_Init" not in txt:
            warns.append("Core/Src/main.c 可能是 CubeMX 生成版而非本仓库业务版；"
                         "请用仓库的 main.c 覆盖（它自带时钟树/IWDG/主循环）")


def main():
    print("== 固件 prebuild 自检（firmware/prebuild_check.py）==")
    errors, warns = [], []
    check_generated_scaffold(errors, warns)
    check_msp_and_it(errors, warns)
    check_cjson(errors, warns)
    check_lcd(errors, warns)
    check_wifi_config(errors, warns)
    check_duplicate_main(errors, warns)

    print("\n[设备宏] 工具链必须定义 %s（HAL 编译硬性前提，CubeMX 工程默认已加）" % DEVICE_MACRO)
    print("[引脚主权] 以下引脚由业务代码初始化，CubeMX 请勿再配置：")
    for pin, who in BUSINESS_OWNED_PINS.items():
        print("   - %s : %s" % (pin, who))

    ok = True
    if warns:
        print("\n[警告 %d 项]" % len(warns))
        for w in warns:
            print("  ! " + w)
    if errors:
        ok = False
        print("\n[错误 %d 项 — 编译前必须解决]" % len(errors))
        for e in errors:
            print("  X " + e)

    print("\n结果：%s" % ("可编译（或无阻塞项，注意警告）" if ok else "存在阻塞项，先按 X 修复"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
