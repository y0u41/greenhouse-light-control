/*
 * eeprom.c 行为测试（主机端，无需硬件）
 *
 * 被测对象是**未经改动的真实 eeprom.c**（firmware/Core/Src/eeprom.c），
 * 通过 tests/host/shim/stm32f1xx_hal.h 垫片在 x86 gcc 上编译。
 *
 * 核心回归锁（N-1「set_th 后 EEPROM_Save FAILED」的软件侧根因）：
 *   write_cycle_busy_then_ack —— 伪 AT24C02 在页写入后 5 ms 内对地址探测
 *   一律**立即** NACK（真实 HAL_I2C_IsDeviceReady 语义）。旧代码的 ACK 轮询
 *   20 次之间无任何间隔，整轮 <<1 ms 耗尽 -> 误报失败；
 *   新代码每次探测后 HAL_Delay(1) 推进 tick，穿过忙窗 -> 成功。
 *   没有这条用例，轮询间隔改动无法被任何现有测试发现/锁住。
 */

#include "stm32f1xx_hal.h"
#include "eeprom.h"
#include <stdio.h>
#include <string.h>

/* ---- 桩接口（stubs_eeprom.c）---- */
void HostEe_Reset(void);
void HostEe_SetInitFail(int on);
void HostEe_SetMemWriteFail(int on);
void HostEe_SetMemReadFail(int on);
void HostEe_SetAlwaysNackReady(int on);
void HostEe_SetPeriphStuck(int on);
void HostEe_SetCorruptNextWrites(int n);
int  HostEe_InitCount(void);
int  HostEe_DeInitCount(void);
int  HostEe_MemWriteCount(void);
int  HostEe_MemReadCount(void);
int  HostEe_ReadyPollCount(void);
int  HostEe_IwdgFeedCount(void);
const uint8_t *HostEe_Raw(void);
void HostEe_Poke(uint8_t addr, uint8_t v);
void HostEe_SetDbg(int on);
int  HostEe_DbgLines(void);

/* ---- 断言框架 ---- */
static int s_pass = 0, s_fail = 0;
static const char *s_group = "";

#define CHECK(cond, fmt, ...) do {                                        \
    if (cond) { s_pass++; }                                               \
    else {                                                                \
        s_fail++;                                                         \
        printf("  [FAIL] %s :: " fmt "\n", s_group, ##__VA_ARGS__);       \
    }                                                                     \
} while (0)

static void group(const char *name) {
    s_group = name;
    printf("\n== %s ==\n", name);
}

/* ---- 用例 ---- */

/* ★ 回归锁：写周期忙窗内探测 NACK，轮询必须靠间隔穿过忙窗。 */
static void test_write_cycle_busy_then_ack(void) {
    group("write_cycle_busy_then_ack（N-1 回归锁）");

    HostEe_Reset();
    HostEe_SetDbg(0);

    /* 伪器件写周期 = 5 ms。没有 HAL_Delay 的旧轮询会在 tick 不变时耗尽。 */
    CHECK(EEPROM_Save(2000, 150) == true,
          "写周期 5ms 内探测 NACK，Save 应靠轮询间隔等到 ACK（实际失败）");
    CHECK(HostEe_MemWriteCount() == 1,
          "一次写入即成功，不应走 force_reinit 重试（实际 %d 次）",
          HostEe_MemWriteCount());
    CHECK(HostEe_ReadyPollCount() >= 5,
          "应发生了多次 ACK 轮询（实际 %d 次）", HostEe_ReadyPollCount());
    CHECK(HostEe_IwdgFeedCount() >= 1,
          "轮询期间应喂狗（实际 %d 次）", HostEe_IwdgFeedCount());

    /* 落盘后 Load 必须读回同样的值 */
    uint16_t th = 0, hys = 0;
    CHECK(EEPROM_Load(&th, &hys) == true && th == 2000 && hys == 150,
          "Save 后 Load 应回读 2000/150（实际 %u/%u）", th, hys);
}

static void test_save_load_roundtrip(void) {
    group("save_load_roundtrip");

    HostEe_Reset();
    CHECK(EEPROM_Save(2500, 200) == true, "Save(2500,200) 应成功");
    uint16_t th = 0, hys = 0;
    CHECK(EEPROM_Load(&th, &hys) == true && th == 2500 && hys == 200,
          "Load 应回读 2500/200（实际 %u/%u）", th, hys);

    /* 边界：th 最大合法值 = LIGHT_ADC_MAX = 4095，hys = 0 */
    CHECK(EEPROM_Save(4095, 0) == true, "Save(4095,0) 边界应成功");
    CHECK(EEPROM_Load(&th, &hys) == true && th == 4095 && hys == 0,
          "Load 应回读 4095/0（实际 %u/%u）", th, hys);
}

static void test_invariant_reject_save(void) {
    group("invariant_reject_save");

    HostEe_Reset();
    const int writes0 = HostEe_MemWriteCount();

    CHECK(EEPROM_Save(150, 150) == false, "hys>=th 应拒绝落盘");
    CHECK(EEPROM_Save(2000, 2000) == false, "hys==th 应拒绝落盘");
    CHECK(EEPROM_Save(4096, 150) == false, "th 超量程(4096>4095) 应拒绝落盘");
    CHECK(HostEe_MemWriteCount() == writes0,
          "不变式拒绝时不应产生任何 I2C 写（实际新增 %d 次）",
          HostEe_MemWriteCount() - writes0);
}

static void test_blank_load_defaults(void) {
    group("blank_load_defaults");

    HostEe_Reset();                          /* 空片 = 全 0xFF */
    uint16_t th = 123, hys = 45;
    CHECK(EEPROM_Load(&th, &hys) == false, "空片 Load 应返回 false");
    CHECK(th == TH_DEFAULT && hys == HYS_DEFAULT,
          "空片应回退默认值 %u/%u（实际 %u/%u）", TH_DEFAULT, HYS_DEFAULT, th, hys);
}

static void test_bad_magic_rejected(void) {
    group("bad_magic_rejected");

    HostEe_Reset();
    CHECK(EEPROM_Save(2000, 150) == true, "前置：Save 应成功");
    HostEe_Poke(0x00, 0x00);                /* 破坏魔数首字节 */

    uint16_t th = 123, hys = 45;
    CHECK(EEPROM_Load(&th, &hys) == false, "魔数破坏后 Load 应返回 false");
    CHECK(th == TH_DEFAULT && hys == HYS_DEFAULT,
          "魔数破坏应回退默认值（实际 %u/%u）", th, hys);
}

static void test_bad_checksum_rejected(void) {
    group("bad_checksum_rejected");

    HostEe_Reset();
    CHECK(EEPROM_Save(2000, 150) == true, "前置：Save 应成功");
    HostEe_Poke(0x07, (uint8_t)(HostEe_Raw()[0x07] ^ 0xFF));  /* 翻转校验低字节 */

    uint16_t th = 123, hys = 45;
    CHECK(EEPROM_Load(&th, &hys) == false, "校验破坏后 Load 应返回 false");
    CHECK(th == TH_DEFAULT && hys == HYS_DEFAULT,
          "校验破坏应回退默认值（实际 %u/%u）", th, hys);
}

static void test_invariant_violating_stored(void) {
    group("invariant_violating_stored");

    /* 构造"校验合法但不变式不满足"的数据：th=100 hys=200（hys>=th）。
     * 若 EEPROM_Load 只查魔数+校验、不复查不变式，就会装载出非法配置。 */
    HostEe_Reset();
    uint8_t crafted[8] = {
        EEPROM_MAGIC0, EEPROM_MAGIC1,
        0x00, 100,      /* th = 100（大端） */
        0x00, 200,      /* hys = 200（大端） */
        0x00, 0x00      /* 校验和，下面算 */
    };
    uint16_t ck = 0;
    for (int i = 0; i < 6; i++) { ck = (uint16_t)(ck + crafted[i]); }
    crafted[6] = (uint8_t)(ck >> 8);
    crafted[7] = (uint8_t)(ck & 0xFF);
    for (int i = 0; i < 8; i++) { HostEe_Poke((uint8_t)i, crafted[i]); }

    uint16_t th = 123, hys = 45;
    CHECK(EEPROM_Load(&th, &hys) == false, "不变式违规数据 Load 应返回 false");
    CHECK(th == TH_DEFAULT && hys == HYS_DEFAULT,
          "不变式违规应回退默认值（实际 %u/%u）", th, hys);
}

static void test_write_corrupt_then_recover(void) {
    group("write_corrupt_then_recover（回读校验 + 重试）");

    /* 第一次写入落盘内容被"写歪"（按位取反）——ACK 正常但回读校验失败。
     * EEPROM_Save 应走 force_reinit + 重试路径，第二次写正确后返回 true。 */
    HostEe_Reset();
    HostEe_SetCorruptNextWrites(1);

    CHECK(EEPROM_Save(2000, 150) == true,
          "首次写歪后应经回读校验+重试恢复（实际仍失败）");
    CHECK(HostEe_MemWriteCount() == 2,
          "应恰好写两次（首次+重试），实际 %d 次", HostEe_MemWriteCount());
    CHECK(HostEe_DeInitCount() == 1,
          "回读失败后应 DeInit 复位外设一次（实际 %d 次）", HostEe_DeInitCount());

    uint16_t th = 0, hys = 0;
    CHECK(EEPROM_Load(&th, &hys) == true && th == 2000 && hys == 150,
          "恢复后 Load 应回读 2000/150（实际 %u/%u）", th, hys);
}

static void test_periph_stuck_recovers(void) {
    group("periph_stuck_recovers（外设锁死恢复）");

    /* 外设级锁死：探测永远 NACK 但器件其实正常（Mem_Write 照常落盘）。
     * 第一次写：轮询耗尽 -> 失败；force_reinit 清锁死；重试 -> 成功。 */
    HostEe_Reset();
    HostEe_SetPeriphStuck(1);

    CHECK(EEPROM_Save(2200, 180) == true,
          "外设锁死时 force_reinit 重试应成功（实际失败）");
    CHECK(HostEe_DeInitCount() == 1, "应 DeInit 一次（实际 %d 次）",
          HostEe_DeInitCount());

    uint16_t th = 0, hys = 0;
    CHECK(EEPROM_Load(&th, &hys) == true && th == 2200 && hys == 180,
          "恢复后 Load 应回读 2200/180（实际 %u/%u）", th, hys);
}

static void test_always_nack_fails(void) {
    group("always_nack_fails（器件级故障不自愈）");

    /* 器件级故障：地址探测永远 NACK（DeInit 也救不了）。
     * 两次尝试（首次 + force_reinit 重试）都应失败，返回 false。 */
    HostEe_Reset();
    HostEe_SetAlwaysNackReady(1);

    CHECK(EEPROM_Save(2000, 150) == false,
          "器件永远 NACK 时 Save 应返回 false");
    CHECK(HostEe_MemWriteCount() == 2,
          "应尝试写两次（首次+重试），实际 %d 次", HostEe_MemWriteCount());
}

static void test_memwrite_fail(void) {
    group("memwrite_fail（传输层写失败）");

    HostEe_Reset();
    HostEe_SetMemWriteFail(1);

    CHECK(EEPROM_Save(2000, 150) == false, "Mem_Write 失败时 Save 应返回 false");
    CHECK(HostEe_ReadyPollCount() == 0, "写传输失败不应进入 ACK 轮询");
}

static void test_memread_fail_load(void) {
    group("memread_fail_load（读失败回退默认值）");

    HostEe_Reset();
    CHECK(EEPROM_Save(2000, 150) == true, "前置：Save 应成功");
    HostEe_SetMemReadFail(1);

    uint16_t th = 123, hys = 45;
    CHECK(EEPROM_Load(&th, &hys) == false, "读失败时 Load 应返回 false");
    CHECK(th == TH_DEFAULT && hys == HYS_DEFAULT,
          "读失败应回退默认值（实际 %u/%u）", th, hys);
}

static void test_init_fail(void) {
    group("init_fail（I2C 初始化失败降级）");

    /* 必须在任何成功 Init 之前跑：s_i2c_ready 初始为 false。 */
    HostEe_Reset();
    HostEe_SetInitFail(1);

    CHECK(EEPROM_Save(2000, 150) == false, "Init 失败时 Save 应返回 false");
    CHECK(HostEe_MemWriteCount() == 0, "Init 失败不应产生 I2C 写");

    uint16_t th = 123, hys = 45;
    /* Init 已失败但 s_i2c_ready=false；Load 仍会尝试读（读也失败）-> 默认值 */
    CHECK(EEPROM_Load(&th, &hys) == false, "Init 失败时 Load 应返回 false");
    CHECK(th == TH_DEFAULT && hys == HYS_DEFAULT,
          "Init 失败 Load 应回退默认值（实际 %u/%u）", th, hys);
    HostEe_SetInitFail(0);                  /* 恢复，避免影响后续 */
}

/* ============================================================ */
int main(void) {
    HostEe_SetDbg(0);
    printf("eeprom.c 行为测试（被测对象：未改动的真实 eeprom.c）\n");

    /* init_fail 必须最先：它依赖 s_i2c_ready 尚未被置位的初始状态 */
    test_init_fail();
    test_write_cycle_busy_then_ack();
    test_save_load_roundtrip();
    test_invariant_reject_save();
    test_blank_load_defaults();
    test_bad_magic_rejected();
    test_bad_checksum_rejected();
    test_invariant_violating_stored();
    test_write_corrupt_then_recover();
    test_periph_stuck_recovers();
    test_always_nack_fails();
    test_memwrite_fail();
    test_memread_fail_load();

    printf("\n================================\n");
    printf("PASS %d   FAIL %d\n", s_pass, s_fail);
    printf("================================\n");
    return s_fail == 0 ? 0 : 1;
}
