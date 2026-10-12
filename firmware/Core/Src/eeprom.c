/**
 * @file eeprom.c
 * @brief AT24C02 掉电记忆：魔数 + 校验和 + 不变式三重校验。
 *
 * 硬件事实（Tech_Design 附录 A.1）：AT24C02 @ I2C1，PB6/PB7，7 位地址 0x50，
 * 硬件 4.7 K 上拉，板载硬连接不可复用。8 字节恰好一页，单次页写入即可。
 *
 * 写周期 ≤5 ms：页写入后用 HAL_I2C_IsDeviceReady 轮询 ACK（每轮喂狗），
 * 不盲等 delay_ms，避免长时间不喂 IWDG。
 */

#include "eeprom.h"
#include "debug_uart.h"   /* Dbg_Printf：降级路径要留下可见记录 */

static I2C_HandleTypeDef s_hi2c1;
static bool s_i2c_ready = false;

#define AT24C02_ADDR        0x50U        /* 7 位地址 */
#define AT24C02_ADDR8_W     (AT24C02_ADDR << 1)
#define EEPROM_DATA_LEN     8U

static uint16_t checksum16(const uint8_t *buf, uint8_t len)
{
    uint16_t sum = 0;
    uint8_t i;

    for (i = 0; i < len; i++) {
        sum += buf[i];
    }
    return sum;
}

static void MX_I2C1_Init(void)
{
    GPIO_InitTypeDef gi = {0};

    if (s_i2c_ready) {
        return;                          /* 幂等：Save/Load 可能先后调用，不重复 Init */
    }
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    gi.Pin   = GPIO_PIN_6 | GPIO_PIN_7;
    gi.Mode  = GPIO_MODE_AF_OD;           /* I2C 必须开漏 + 外部上拉 */
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gi);

    s_hi2c1.Instance             = I2C1;
    s_hi2c1.Init.ClockSpeed      = 100000;              /* 标准模式 100 kHz */
    s_hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    s_hi2c1.Init.OwnAddress1     = 0;
    s_hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    s_hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    s_hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    s_hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&s_hi2c1) != HAL_OK) {
        /* ★ 不再调用 Error_Handler()：它是 __disable_irq() + 死循环，
         * 而 EEPROM 初始化发生在 IWDG 启动**之前**（见 main.c 的启动顺序），
         * 于是"死循环 + 狗没开" = 永久挂死、连复位都不会有。
         * 正确做法是降级：不置 s_i2c_ready，后续读写因读不到而回退默认值，
         * 本地闭环（采集/显示/控制）完全不受影响。 */
        Dbg_Printf("[eeprom] I2C1 init FAILED (non-fatal, fall back to defaults)\r\n");
        s_i2c_ready = false;
        return;
    }
    s_i2c_ready = true;
}

static bool at24c02_read(uint8_t addr, uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(&s_hi2c1, AT24C02_ADDR8_W, addr,
                            I2C_MEMADD_SIZE_8BIT, buf, len, 100) == HAL_OK;
}

/* 页写入 + ACK 轮询（每 5 次喂一次狗，写周期约 5 ms） */
static bool at24c02_write(uint8_t addr, const uint8_t *buf, uint16_t len)
{
    if (HAL_I2C_Mem_Write(&s_hi2c1, AT24C02_ADDR8_W, addr,
                          I2C_MEMADD_SIZE_8BIT, (uint8_t *)buf, len, 100) != HAL_OK) {
        return false;
    }
    for (uint8_t i = 0; i < 20U; i++) {
        if (HAL_I2C_IsDeviceReady(&s_hi2c1, AT24C02_ADDR8_W, 1, 5) == HAL_OK) {
            return true;                  /* ACK = 写入完成 */
        }
        if ((i % 5U) == 4U) {
            Iwdg_Feed();
        }
    }
    return false;
}

bool EEPROM_Save(uint16_t th, uint16_t hys)
{
    uint8_t buf[EEPROM_DATA_LEN];

    if (hys >= th || th > LIGHT_ADC_MAX) {
        return false;                      /* 不变式不满足，拒绝落盘 */
    }

    buf[0] = EEPROM_MAGIC0;
    buf[1] = EEPROM_MAGIC1;
    buf[2] = (uint8_t)(th >> 8);
    buf[3] = (uint8_t)(th & 0xFFU);
    buf[4] = (uint8_t)(hys >> 8);
    buf[5] = (uint8_t)(hys & 0xFFU);

    uint16_t ck = checksum16(buf, 6);
    buf[6] = (uint8_t)(ck >> 8);
    buf[7] = (uint8_t)(ck & 0xFFU);

    MX_I2C1_Init();                        /* 幂等：首次调用才真正初始化 */
    return at24c02_write(0x00, buf, EEPROM_DATA_LEN);
}

bool EEPROM_Load(uint16_t *th, uint16_t *hys)
{
    uint8_t buf[EEPROM_DATA_LEN];
    uint16_t t;
    uint16_t h;

    /* 先落默认值，任何一步失败都保持默认，不阻塞启动 */
    *th  = TH_DEFAULT;
    *hys = HYS_DEFAULT;

    MX_I2C1_Init();

    if (!at24c02_read(0x00, buf, EEPROM_DATA_LEN)) {
        return false;                      /* 读失败 */
    }
    if (buf[0] != EEPROM_MAGIC0 || buf[1] != EEPROM_MAGIC1) {
        return false;                      /* 魔数不符（空片全 0xFF） */
    }
    if (checksum16(buf, 6) != (uint16_t)(((uint16_t)buf[6] << 8) | buf[7])) {
        return false;                      /* 校验失败 */
    }

    t = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    h = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
    if (h >= t || t > LIGHT_ADC_MAX) {
        return false;                      /* 不变式不满足，回退默认值 */
    }

    *th  = t;
    *hys = h;
    return true;
}
