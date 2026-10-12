/**
 * @file lcd_show.c
 * @brief 本地液晶显示：光强 / 阈值 / 执行器 / 模式。
 *
 * 依赖官方 BSP 的 lcd.h（精英版 V2 资料盘，NT35310，FSMC 16 位并口，480×320）。
 *
 * 注意官方 V2 BSP 的 API 形态与早期寄存器版不同，本文件按 V2 实际声明调用：
 *   void lcd_init(void);
 *   void lcd_clear(uint16_t color);
 *   void lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
 *                        uint8_t size, char *p, uint16_t color);      <- 7 参、带颜色、函数名小写
 *   void lcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len,
 *                     uint8_t size, uint16_t color);
 * 另有全局 "g_back_color"（默认白色）：lcd_show_string 用它填充字符串右侧剩余宽度，
 * 因此清屏与刷新前必须保证它是白色，否则会拖出彩色残影。
 *
 * 若本地工程未引入 BSP，把 main.h 的 LCD_SHOW_ENABLED 置 0，本模块编译为桩，
 * 采集 / 控制 / 上报 / 本地闭环完全不受影响。
 */

#include "lcd_show.h"

#if LCD_SHOW_ENABLED

#include "lcd.h"
#include "debug_uart.h"

/* 行坐标（480×320 横屏） */
#define LCD_ROW_Y_TITLE     20
#define LCD_ROW_Y_LIGHT     70
#define LCD_ROW_Y_TH       100
#define LCD_ROW_Y_RELAY    130
#define LCD_ROW_Y_MODE     160
#define LCD_LABEL_X         20
#define LCD_LABEL_W        110
#define LCD_VALUE_X        130

void LcdShow_Init(void)
{
    lcd_init();

    /* ---- 关键修复：必须显式切成横屏 ----
     * lcd_init() 末尾只执行到 lcd_display_dir(0)，即**竖屏 240×320**；
     * 而本文件的全部坐标是按**横屏 480×320** 设计的（标题宽度传 440、
     * LCD_VALUE_X = 130）。两者不匹配时 lcd_show_string 会把字符写到 x = 240~460
     * 的**屏外区域**，越界写 GRAM —— 表现就是整屏显示错乱/不断变化。
     * 参考工程在 User/App/app_main.c:1096 明确调用了 lcd_display_dir(1)，我们漏了这一步。
     * 顺序很重要：**先切方向，再清屏**，否则清屏按旧尺寸做。 */
    lcd_display_dir(1);                 /* 1 = 横屏 480×320 */

    g_back_color = WHITE;               /* 字符串右侧填充色，必须与背景一致 */
    lcd_clear(WHITE);

    /* 照抄参考工程 app_main.c:1100-1103：把屏型/分辨率打出来，一眼确认识别是否正确。
     * 若 id=0 或宽高不是 480×320，说明 lcd_init() 没认对屏，显示问题就另有原因。 */
#if DEBUG_UART
    Dbg_Printf("[lcd] id=0x%x width=%u height=%u dir=1\r\n",
               (unsigned)lcddev.id, (unsigned)lcddev.width, (unsigned)lcddev.height);
#endif

    lcd_show_string(LCD_LABEL_X, LCD_ROW_Y_TITLE, 440, 24, 24,
                    (char *)"Greenhouse Light Ctrl", BLUE);
    lcd_show_string(LCD_LABEL_X, LCD_ROW_Y_LIGHT, LCD_LABEL_W, 20, 16, (char *)"Light:", BLACK);
    lcd_show_string(LCD_LABEL_X, LCD_ROW_Y_TH,    LCD_LABEL_W, 20, 16, (char *)"Th   :", BLACK);
    lcd_show_string(LCD_LABEL_X, LCD_ROW_Y_RELAY, LCD_LABEL_W, 20, 16, (char *)"Relay:", BLACK);
    lcd_show_string(LCD_LABEL_X, LCD_ROW_Y_MODE,  LCD_LABEL_W, 20, 16, (char *)"Mode :", BLACK);
}

void LcdShow_Update(uint16_t light, uint16_t th, uint8_t relay, uint8_t mode)
{
    /* 光强 / 阈值：固定 4 位宽度，避免上次的长数字残留末位 */
    lcd_show_num(LCD_VALUE_X, LCD_ROW_Y_LIGHT, light, 4, 16, BLACK);
    lcd_show_num(LCD_VALUE_X, LCD_ROW_Y_TH,    th,    4, 16, BLACK);

    /* 执行器：吸合时变红，演示时一眼可见"遮阳动作" */
    lcd_show_string(LCD_VALUE_X, LCD_ROW_Y_RELAY, 60, 20, 16,
                    (char *)(relay ? "ON " : "OFF"), relay ? RED : BLACK);

    /* 模式：两种取值都补足 6 字符，避免 "MANUAL" -> "AUTO  " 时残留 "L" */
    lcd_show_string(LCD_VALUE_X, LCD_ROW_Y_MODE, 90, 20, 16,
                    (char *)(mode ? "MANUAL" : "AUTO  "), BLACK);
}

#else  /* LCD_SHOW_ENABLED == 0：编译桩 */

void LcdShow_Init(void) { }

void LcdShow_Update(uint16_t light, uint16_t th, uint8_t relay, uint8_t mode)
{
    (void)light; (void)th; (void)relay; (void)mode;
}

#endif /* LCD_SHOW_ENABLED */
