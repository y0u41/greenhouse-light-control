#ifndef HOST_SHIM_STM32F1XX_HAL_H
#define HOST_SHIM_STM32F1XX_HAL_H
/*
 * 主机测试用的极小 HAL 垫片。
 *
 * 为什么不直接用真实 HAL 头：stm32f1xx.h -> core_cm3.h 里有大量 ARM 内联汇编
 * （__NOP/__WFI/__ASM 等）。x86 的 gcc 在**语法解析**阶段就会报错，
 * 即使那些 static inline 函数从未被调用。
 *
 * 这个垫片只提供 control.c / protocol.c 真正用到的类型、宏与函数声明，
 * 使**未经改动的真实源码**能在 PC 上编译 —— 这是本测试方案能成立的关键：
 * 被测的是真代码，不是复制品。
 *
 * 把它放在 -I 的**最前面**，让 "stm32f1xx_hal.h" 命中本文件。
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HAL_OK = 0x00U, HAL_ERROR = 0x01U, HAL_BUSY = 0x02U,
               HAL_TIMEOUT = 0x03U } HAL_StatusTypeDef;

typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

/* 假端口：control.c 只把地址透传给桩函数，从不真正解引用 */
typedef struct { volatile uint32_t dummy; } GPIO_TypeDef;

extern GPIO_TypeDef s_fake_gpio_a, s_fake_gpio_b, s_fake_gpio_d, s_fake_gpio_e;
#define GPIOA (&s_fake_gpio_a)
#define GPIOB (&s_fake_gpio_b)
#define GPIOD (&s_fake_gpio_d)
#define GPIOE (&s_fake_gpio_e)

typedef struct {
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
    uint32_t Speed;
} GPIO_InitTypeDef;

#define GPIO_PIN_0    ((uint16_t)0x0001)
#define GPIO_PIN_5    ((uint16_t)0x0020)
#define GPIO_PIN_6    ((uint16_t)0x0040)
#define GPIO_PIN_7    ((uint16_t)0x0080)
#define GPIO_PIN_8    ((uint16_t)0x0100)
#define GPIO_PIN_11   ((uint16_t)0x0800)

#define GPIO_MODE_OUTPUT_PP    ((uint32_t)0x00000001U)
#define GPIO_MODE_AF_OD        ((uint32_t)0x00000012U)
#define GPIO_MODE_ANALOG       ((uint32_t)0x00000003U)
#define GPIO_NOPULL            ((uint32_t)0x00000000U)
#define GPIO_SPEED_FREQ_LOW    ((uint32_t)0x00000002U)
#define GPIO_SPEED_FREQ_MEDIUM ((uint32_t)0x00000001U)
#define GPIO_SPEED_FREQ_HIGH   ((uint32_t)0x00000003U)

/* ---- I2C（eeprom.c 需要；control/protocol 套件不会引用这些符号） ---- */
typedef struct {
    uint32_t ClockSpeed;
    uint32_t DutyCycle;
    uint32_t OwnAddress1;
    uint32_t AddressingMode;
    uint32_t DualAddressMode;
    uint32_t GeneralCallMode;
    uint32_t NoStretchMode;
} I2C_InitTypeDef;

typedef struct {
    void *Instance;              /* eeprom.c 只赋值/透传，从不解引用 */
    I2C_InitTypeDef Init;
} I2C_HandleTypeDef;

extern I2C_HandleTypeDef s_fake_i2c1;
#define I2C1 (&s_fake_i2c1)

#define I2C_DUTYCYCLE_2          ((uint32_t)0x00000000U)
#define I2C_ADDRESSINGMODE_7BIT  ((uint32_t)0x00000001U)
#define I2C_DUALADDRESS_DISABLE  ((uint32_t)0x00000000U)
#define I2C_GENERALCALL_DISABLE  ((uint32_t)0x00000000U)
#define I2C_NOSTRETCH_DISABLE    ((uint32_t)0x00000000U)
#define I2C_MEMADD_SIZE_8BIT     ((uint16_t)0x00000001U)

/* 时钟使能宏：主机上无意义，设为空操作；但保留以便断言"是否被调用过" */
void HostStub_RecordClkEnable(void);
#define __HAL_RCC_GPIOA_CLK_ENABLE()  do { HostStub_RecordClkEnable(); } while (0)
#define __HAL_RCC_GPIOB_CLK_ENABLE()  do { HostStub_RecordClkEnable(); } while (0)
#define __HAL_RCC_GPIOD_CLK_ENABLE()  do { HostStub_RecordClkEnable(); } while (0)
#define __HAL_RCC_GPIOE_CLK_ENABLE()  do { HostStub_RecordClkEnable(); } while (0)
#define __HAL_RCC_GPIOF_CLK_ENABLE()  do { HostStub_RecordClkEnable(); } while (0)
#define __HAL_RCC_I2C1_CLK_ENABLE()   do { HostStub_RecordClkEnable(); } while (0)

/* 被桩替换的真实 HAL 入口 */
uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t ms);
void     HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void     HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st);

HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize,
                                   uint8_t *pData, uint16_t Size, uint32_t Timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize,
                                    uint8_t *pData, uint16_t Size, uint32_t Timeout);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                        uint32_t Trials, uint32_t Timeout);

/* 其它模块偶尔引用、但本测试不关心的 HAL 符号 */
uint32_t HAL_RCC_GetHCLKFreq(void);

/* 临界区原语（真机上由 CMSIS 的 cmsis_gcc.h 提供 cpsid/cpsie 指令）。
 * 主机端是单线程，做成空操作即可。
 * ⚠ 注意：这让"临界区是否真的挡住了并发访问"在**主机测试里测不出来** ——
 * 那类问题只能在真机上验证（参见 AGENTS.md 的并发/共享变量陷阱）。 */
static inline void __disable_irq(void) { }
static inline void __enable_irq(void)  { }
static inline void __NOP(void)         { }
static inline void __WFI(void)         { }

#ifdef __cplusplus
}
#endif
#endif /* HOST_SHIM_STM32F1XX_HAL_H */
