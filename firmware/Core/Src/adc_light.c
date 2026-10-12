/**
 * @file adc_light.c
 * @brief 光照采集：PF8(ADC3_IN6) 采样、均值滤波、light 换算。
 *
 * 实现要点（对应 Tech_Design §采样与滤波）：
 *  - 单次转换 252 个 ADC 周期 = 21 µs（采样 239.5 + 转换 12.5，ADCCLK 12 MHz）；
 *  - 自写 8 次均值循环，每样本间不插 delay_ms，整体 ≈0.17 ms；
 *    官方 adc3_get_result_average() 每样本 delay_ms(5)，8 次阻塞约 40 ms，本项目不用；
 *  - 不直接用官方 lsens_get_val()：它把 4096 级压成 100 级（分辨率 2.44%），
 *    与协议声明的 0–4095 不符；本模块返回原始码值。
 */

#include "adc_light.h"
#include "debug_uart.h"

static ADC_HandleTypeDef s_hadc3;

/* ---- 采样有效性（修复"传感器故障伪装成最强光照"）----
 * 原实现采样失败时 `return 0`，而 0 经 LIGHT_FROM_ADC 变成 **4095 = 最强光**，
 * 于是 control.c 判定"光过强"→ 吸合执行器 + 响铃 + 上报 alarm。
 * "读失败"和"最亮"共用一个值，就是把故障翻译成了报警。
 * 现在：失败时**保持上次有效值**并置 s_valid=0，由上层自行决定是否信任。
 * 另外 LightSensor_Init 的失败也**不再调用 Error_Handler**：
 * 那会 __disable_irq() + 死循环，而此时 IWDG 尚未启动（它在 main 末尾），
 * 结果是永久挂死且连复位都没有。 */
static uint16_t s_last_good = 0U;   /* 上次有效均值 */
static uint8_t  s_valid     = 0U;   /* 1 = 本次读数可信 */

#define ADC_LIGHT_INVALID   0xFFFFU /* 单次采样失败哨兵（12 位量程内不可能出现） */

/**
 * @brief ADC3 + PF8 初始化。
 * PF8 = ADC3_IN6（官方 lsens.h：LSENS_ADC3_CHX = ADC_CHANNEL_6 / GPIO_PIN_8）。
 * 官方 adc3_init() 只配 PA1，不会配 PF8，因此这里自行完成 GPIOF 时钟与模拟输入配置。
 * **全部失败路径均为降级**：置 s_valid=0、打印原因、继续跑本地其它功能。
 */
static void MX_ADC3_Init(void)
{
    GPIO_InitTypeDef gi = {0};
    ADC_ChannelConfTypeDef ch = {0};

    /* ADC3 时钟挂在 APB2；GPIOF 时钟必须先使能 */
    __HAL_RCC_ADC3_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    gi.Pin  = GPIO_PIN_8;
    gi.Mode = GPIO_MODE_ANALOG;          /* 模拟输入，无需上下拉 */
    HAL_GPIO_Init(GPIOF, &gi);

    s_hadc3.Instance                   = ADC3;
    s_hadc3.Init.ScanConvMode          = DISABLE;                 /* 单通道，不用扫描 */
    s_hadc3.Init.ContinuousConvMode    = DISABLE;                 /* 软件触发，单次转换 */
    s_hadc3.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    s_hadc3.Init.NbrOfConversion       = 1;
    s_hadc3.Init.DiscontinuousConvMode = DISABLE;
    s_hadc3.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    if (HAL_ADC_Init(&s_hadc3) != HAL_OK) {
        s_valid = 0U;
        Dbg_Printf("[adc] HAL_ADC_Init FAILED (non-fatal)\r\n");
        return;
    }

    ch.Channel      = ADC_CHANNEL_6;                     /* PF8 = ADC3_IN6 */
    ch.Rank         = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;        /* 239.5 周期，与官方 adc3 一致 */
    if (HAL_ADC_ConfigChannel(&s_hadc3, &ch) != HAL_OK) {
        s_valid = 0U;
        Dbg_Printf("[adc] ConfigChannel FAILED (non-fatal)\r\n");
        return;
    }

    /* 自校准：F1 ADC 上电后必须校准，否则码值有偏移。
     * 校准失败**不致命**（只是精度下降），仍可继续采样。 */
    if (HAL_ADCEx_Calibration_Start(&s_hadc3) != HAL_OK) {
        Dbg_Printf("[adc] calibration FAILED (precision degraded, non-fatal)\r\n");
    }
}

void LightSensor_Init(void)
{
    MX_ADC3_Init();
}

/**
 * @brief 单次 ADC3 采样，返回 12 位原始码值（0–4095，越小越亮）。
 * 软件触发一次转换，21 µs 内完成，不阻塞主循环。
 */
static uint16_t adc3_get_result(void)
{
    uint32_t value = 0;

    if (HAL_ADC_Start(&s_hadc3) != HAL_OK) {
        return ADC_LIGHT_INVALID;          /* 不再 return 0（0 会被当成"最亮"） */
    }
    if (HAL_ADC_PollForConversion(&s_hadc3, 1) != HAL_OK) {
        (void)HAL_ADC_Stop(&s_hadc3);
        return ADC_LIGHT_INVALID;
    }
    value = HAL_ADC_GetValue(&s_hadc3);
    (void)HAL_ADC_Stop(&s_hadc3);
    return (uint16_t)(value & 0x0FFFU);
}

uint16_t Light_ReadRaw(uint8_t times)
{
    uint32_t sum = 0U;
    uint8_t  ok  = 0U;
    uint16_t v;

    if (times == 0U) {
        times = 1U;
    }
    for (uint8_t i = 0; i < times; i++) {
        v = adc3_get_result();
        if (v != ADC_LIGHT_INVALID) {      /* 只对有效样本求平均 */
            sum += v;
            ok++;
        }
    }

    if (ok == 0U) {
        /* 本次全部失败：**保持上次有效值**，并标记不可信。
         * 上层（control.c）据此跳过自动决策，宁可"维持现状"也不误动作。 */
        s_valid = 0U;
        return s_last_good;
    }

    s_last_good = (uint16_t)(sum / ok);
    s_valid     = 1U;
    return s_last_good;
}

/**
 * @brief 本次读数是否可信。
 * 返回 0 表示采样失败（传感器脱线/ADC 异常），调用方**不应据此驱动执行器**。
 */
uint8_t Light_IsValid(void)
{
    return s_valid;
}
