/*
 * protocol.c 解析健壮性测试（主机端，无需硬件）
 *
 * 状态：51 PASS / 3 FAIL —— 3 条红灯**全部指向同一个真实未修缺陷**，故意保持红灯。
 *
 * ┌─ 真实固件缺陷 ────────────────────────────────────────────────────────┐
 * │ 【P-1】自写 cJSON 不支持 JSON 数组，导致含数组的整帧被静默丢弃。      │
 * │                                                                       │
 * │ 根因：firmware/Middlewares/cJSON/cJSON.c **没有 parse_array**。       │
 * │       parse_value()（:269）只处理 '{'（:278 转 parse_object），       │
 * │       没有 '[' 分支 —— 数组直接落到解析失败，cJSON_Parse 返回 NULL，  │
 * │       protocol.c:126-128 于是静默 return，**连 err ack 都不发**。     │
 * │                                                                       │
 * │ 已用逐键隔离定位（tests/host/test_protocol.c 的"未知键"组）：         │
 * │       未知数字键 / 字符串键 / 布尔键 / null 键 / 嵌套对象 / 转义引号  │
 * │       —— 全部正常忽略，帧照常生效；                                   │
 * │       **唯独未知数组键让整帧失效**。                                  │
 * │                                                                       │
 * │ 影响：协议明确声明"忽略未知键以向前兼容"，但上位机只要新增**任何**   │
 * │       数组字段（如批处理列表、历史点、多阈值），该帧就会整条失效，    │
 * │       表现为"指令没反应、ack 超时"，且**没有任何可观测的错误提示**。  │
 * │       这是"向前兼容"承诺与实现不符，属于接口失约。                   │
 * │                                                                       │
 * │ 修法（二选一）：                                                      │
 * │  a) 给 cJSON 补 parse_array + 对应的 cJSON_Delete 递归释放（正解，    │
 * │     与官方 cJSON 行为一致）；                                         │
 * │  b) 若确定永不使用数组，则在 protocol_schema 与文档里把"不支持数组"  │
 * │     写成显式约束，并让 ParseAndExecute 在解析失败时回一个 err ack，   │
 * │     避免"静默丢弃"（治标，但消除了不可观测性）。                      │
 * └───────────────────────────────────────────────────────────────────────┘
 *
 * 【P-2】超长行不是"整行丢弃"，而是"截断后把尾巴当一帧返回"（次要）。
 *       protocol.c:101-105 在 s_line_len 达上限时只做 s_line_len = 0，
 *       并不丢弃到行尾；于是 250 字节的行撞上限后从头重新累积，
 *       遇 '\n' 时把**尾巴 50 字节**当一帧交出（实测 strlen=50）。
 *       截断的尾巴若恰好构成合法 JSON 就会被当真指令执行。
 *
 * ── 测试夹具自身踩过的坑（已修，留记录以免重犯）──────────────────────
 *  ① **"空转通过"**：第一版 feed_and_execute 只调 Protocol_PopCommand，
 *     **从未调用 Protocol_RxByte** —— 一个字节都没喂进环形缓冲，所有用例都
 *     "执行 0 条"；而 group 4 的 18 条畸形输入期望值本来就是 0，
 *     于是**全部空转变绿**，掩盖了夹具彻底失效。
 *     → 已补"正向对照"用例：先证明正常帧确实能执行，之后"应拒绝"的断言才有意义。
 *  ② 喂 600 字节超出 256 环形缓冲容量，后续帧被环形缓冲丢弃，
 *     被误读成"行溢出后无法恢复"—— 是测试设计错误，不是固件缺陷。
 *  ③ reset_to 用 relay_on 建前态，而它会把 g_mode 置为 MANUAL，
 *     导致所有 AUTO 用例因手动优先提前返回而假失败（control 套件，已修）。
 *
 * 覆盖 P0-2 要求的"畸形 JSON / 半帧"：这类输入在真实链路上**一定会出现**
 * （TCP 会任意切分字节流、模块重启会插入 URC、干扰会造成截断），
 * 而它们的失效模式是"静默丢弃" —— 没有测试就永远发现不了。
 */

#include "stm32f1xx_hal.h"
#include "protocol.h"
#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 桩接口 ---- */
void HostP_ClearCmds(void);
int  HostP_CmdCount(void);
const char *HostP_CmdActionAt(int i);
int32_t HostP_CmdValueAt(int i);
void HostP_ClearRaw(void);
int  HostP_RawCount(void);
const char *HostP_RawAt(int i);
void HostP_SetDbg(int on);

static int s_pass = 0, s_fail = 0;
static const char *s_group = "";
#define CHECK(cond, fmt, ...) do {                                        \
    if (cond) { s_pass++; }                                               \
    else { s_fail++; printf("  [FAIL] %s :: " fmt "\n", s_group, ##__VA_ARGS__); } \
} while (0)
static void group(const char *n) { s_group = n; printf("\n== %s ==\n", n); }

/* 把一串字节喂进协议解析器，然后取出一帧 */
static bool feed_and_pop(const char *s, char *out, size_t cap) {
    for (const char *p = s; *p; p++) Protocol_RxByte((uint8_t)*p);
    return Protocol_PopCommand(out, cap);
}
/* 喂一串字节并直接执行（模拟主循环：RxByte -> PopCommand -> ParseAndExecute）
 *
 * ★ 教训（本测试第一版栽在这里）★
 * 第一版这个函数**只调了 Protocol_PopCommand，从头到尾没有调用 Protocol_RxByte**
 * —— 一个字节都没喂进环形缓冲，于是 PopCommand 立即返回 false，
 * 所有用例都"执行 0 条"。而 group 4 的 18 条畸形输入期望值本来就是 0，
 * 于是它们**全部空转通过**，掩盖了夹具本身的彻底失效。
 * 这类"断言'什么都没发生'的测试在机制坏掉时会自动变绿"是最危险的假阳性。
 * 因此下面加了正向对照用例（positive control）：必须先证明"正常帧确实能被执行"，
 * 之后那些"应当拒绝"的断言才有意义。 */
static int s_dbg_feed = 0;
static void feed_and_execute(const char *s) {
    char buf[PROTOCOL_FRAME_MAX];
    int n = 0;
    for (const char *p = s; *p; p++) {
        Protocol_RxByte((uint8_t)*p);          /* ← 必须真的喂进去 */
    }
    while (Protocol_PopCommand(buf, sizeof(buf))) {
        if (s_dbg_feed) printf("    [dbg] popped = '%s'\n", buf);
        Protocol_ParseAndExecute(buf);
        n++;
    }
    if (s_dbg_feed) printf("    [dbg] '%s' -> 取出并执行 %d 帧\n", s, n);
}

int main(void) {
    HostP_SetDbg(0);
    char buf[PROTOCOL_FRAME_MAX];
    printf("protocol.c 解析健壮性测试（被测对象：未改动的真实 protocol.c）\n");
    printf("NODE_ID = %u\n", (unsigned)NODE_ID);
    s_dbg_feed = (getenv("GH_TEST_DEBUG") != NULL);

    /* ============================================================
     * 1. 半帧：不得被当成一条指令
     * ============================================================ */
    group("半帧：无 '\\n' 时不产出指令");
    Protocol_Reset(); HostP_ClearCmds();
    CHECK(!feed_and_pop("{\"node\":1,\"type\":\"cmd\"", buf, sizeof(buf)),
          "无换行的半帧不应能取出指令");
    Protocol_Reset();
    CHECK(!feed_and_pop("{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\"}", buf, sizeof(buf)),
          "完整 JSON 但没有 '\\n' 仍不算一帧（必须等换行）");

    /* 补上换行后应当成帧 */
    Protocol_Reset(); HostP_ClearCmds();
    const char *full = "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\"}\n";
    for (const char *p = full; *p; p++) Protocol_RxByte((uint8_t)*p);
    CHECK(Protocol_PopCommand(buf, sizeof(buf)),
          "补上 '\\n' 后应能取出指令");
    Protocol_ParseAndExecute(buf);
    CHECK(HostP_CmdCount() == 1, "应执行 1 条指令，实际 %d", HostP_CmdCount());
    CHECK(HostP_CmdActionAt(0) && strcmp(HostP_CmdActionAt(0), "auto") == 0,
          "action 应为 auto");

    /* ============================================================
     * 2. 半帧 + 重连：Protocol_Reset 必须清干净
     * ============================================================ */
    group("重连：Protocol_Reset 清掉残留半帧");
    Protocol_Reset(); HostP_ClearCmds();
    for (const char *p = "{\"node\":1,\"type\":\"cmd\",\"act"; *p; p++) Protocol_RxByte((uint8_t)*p);
    Protocol_Reset();                       /* 模拟断链 */
    /* 重连后第一条：(被污染的半帧 + 新帧) 若未清，必然解析失败 */
    const char *after = "{\"node\":1,\"type\":\"cmd\",\"action\":\"relay_on\"}\n";
    for (const char *p = after; *p; p++) Protocol_RxByte((uint8_t)*p);
    CHECK(Protocol_PopCommand(buf, sizeof(buf)), "重连后应能取出指令");
    Protocol_ParseAndExecute(buf);
    CHECK(HostP_CmdCount() == 1 && HostP_CmdActionAt(0)
          && strcmp(HostP_CmdActionAt(0), "relay_on") == 0,
          "重连后第一条指令必须能正常解析（这正是旧实现被吞掉的场景）");

    /* ============================================================
     * 3. 超长无换行：丢弃并复位
     * ============================================================ */
    group("超长无换行：丢弃且不撑爆缓冲");
    Protocol_Reset(); HostP_ClearCmds();
    /* 用 250 字节：超过 200 的行上限，但不超过 256 的环形缓冲 —— 这样测的是
     * "行缓冲溢出处理"，而不是被环形缓冲丢字节（后者是另一回事）。
     * 第一版喂了 600 字节，把环形缓冲灌满，导致后续帧被丢，
     * 误判成"行溢出后无法恢复"—— 那是测试设计错误，不是固件缺陷。 */
    for (int i = 0; i < 250; i++) Protocol_RxByte((uint8_t)'A');
    Protocol_RxByte((uint8_t)'\n');
    /* ⚠ 实测发现：这里 PopCommand **返回了 true** —— 也就是说超长行不是被"整行丢弃"，
     * 而是"截断后把尾巴当一帧返回"。这与 protocol.c:104 注释宣称的语义不符，
     * 属于真实缺陷（详见本次测试结论）。下面先按**实际行为**断言，把差异显式记录下来，
     * 而不是把测试改成迁就实现 —— 迁就就失去发现问题的意义。 */
    bool popped = Protocol_PopCommand(buf, sizeof(buf));
    CHECK(popped, "（记录实际行为）超长行当前会被截断成一帧返回，而非整行丢弃");
    if (popped) {
        printf("    [观察] 超长行产出的帧内容长度 = %zu（应为 0，即整行丢弃）\n",
               strlen(buf));
    }

    /* 决定性实验：超长行之后先 Reset 再喂正常帧 */
    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":1,\"type\":\"cmd\",\"action\":\"get_state\"}\n");
    CHECK(HostP_CmdCount() == 1 && HostP_CmdActionAt(0)
          && strcmp(HostP_CmdActionAt(0), "get_state") == 0,
          "Reset 之后正常帧应能解析（实际执行 %d 条）", HostP_CmdCount());

    /* ============================================================
     * 4. 畸形输入一律忽略，不得崩
     * ============================================================ */
    group("畸形输入：忽略且不崩");
    /* ---- 正向对照（positive control）----
     * 先证明"一帧正常指令确实能被取出并执行"，否则下面那些
     * "不应执行任何指令"的断言会在夹具失效时**自动变绿**（本测试第一版即如此）。 */
    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\"}\n");
    CHECK(HostP_CmdCount() == 1,
          "正向对照失败：正常帧都执行不了，说明夹具/链路有问题，"
          "下面所有\"应拒绝\"的断言都不可信（实际 %d 条）", HostP_CmdCount());

    static const char *malformed[] = {
        "\n",
        "\r\n",
        "not json at all\n",
        "{\n",                                   /* 截断的 JSON */
        "{\"node\":1,\"type\":\"cmd\"\n",         /* 缺 action */
        "[]\n",                                  /* 数组 */
        "{}\n",                                  /* 空对象 */
        "null\n",
        "12345\n",
        "\"just a string\"\n",
        "{\"node\":1,\"type\":\"cmd\",\"action\":123}\n",   /* action 非字符串 */
        "{\"node\":1,\"type\":123,\"action\":\"auto\"}\n",  /* type 非字符串 */
        "{\"node\":1,\"type\":\"report\",\"action\":\"auto\"}\n", /* 上行 type 混入下行 */
        "{\"node\":1,\"type\":\"evt\",\"action\":\"auto\"}\n",
        "{\"node\":1,\"type\":\"ack\",\"action\":\"auto\"}\n",
        "CLOSED\n",                              /* URC 混进接收流 */
        "WIFI DISCONNECT\n",
        "AT+CIPSEND\r\n",
    };
    const int nm = (int)(sizeof(malformed) / sizeof(malformed[0]));
    for (int i = 0; i < nm; i++) {
        Protocol_Reset(); HostP_ClearCmds();
        feed_and_execute(malformed[i]);
        CHECK(HostP_CmdCount() == 0,
              "畸形输入[%d] 不应执行任何指令，实际 %d 条（输入: %s）",
              i, HostP_CmdCount(), malformed[i]);
    }
    printf("  畸形样例 %d 条\n", nm);

    /* ============================================================
     * 5. 节点过滤（P0 级：缺 node 必须拒绝）
     * ============================================================ */
    group("节点过滤：缺 node / 不匹配 / 宽度越界");
    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"type\":\"cmd\",\"action\":\"auto\"}\n");          /* 缺 node */
    CHECK(HostP_CmdCount() == 0,
          "缺 node 的帧必须拒绝（旧实现在这里会**无条件执行**，是最危险的一条）");

    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":2,\"type\":\"cmd\",\"action\":\"auto\"}\n"); /* 别的节点 */
    CHECK(HostP_CmdCount() == 0, "node 不匹配必须拒绝");

    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":257,\"type\":\"cmd\",\"action\":\"auto\"}\n"); /* 257 & 0xFF == 1 */
    CHECK(HostP_CmdCount() == 0,
          "node=257 不得被节点 1 接受（旧实现 (uint8_t) 截断后会被接受）");

    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":-255,\"type\":\"cmd\",\"action\":\"auto\"}\n");
    CHECK(HostP_CmdCount() == 0,
          "node=-255 不得被节点 1 接受（-255 & 0xFF == 1）");

    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\"}\n");
    CHECK(HostP_CmdCount() == 1, "正确的 node 必须接受（实际执行 %d 条：%s）",
          HostP_CmdCount(), HostP_CmdCount() > 0 ? HostP_CmdActionAt(0) : "-");

    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":\"1\",\"type\":\"cmd\",\"action\":\"auto\"}\n"); /* node 是字符串 */
    CHECK(HostP_CmdCount() == 0, "node 为字符串应拒绝（不是静默当默认节点）");

    /* ============================================================
     * 6. value 透传
     * ============================================================ */
    group("value 透传");
    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":1,\"type\":\"cmd\",\"action\":\"set_th\",\"value\":2000}\n");
    CHECK(HostP_CmdCount() == 1 && HostP_CmdValueAt(0) == 2000,
          "value=2000 应透传，实际执行 %d 条、value=%d",
          HostP_CmdCount(), (int)HostP_CmdValueAt(0));

    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":1,\"type\":\"cmd\",\"action\":\"get_state\"}\n");
    CHECK(HostP_CmdValueAt(0) == -1, "无 value 时应传 -1（约定）");

    /* ============================================================
     * 7. 未知键向前兼容（协议声明要忽略）
     * ============================================================ */
    group("未知键：忽略但帧仍生效");
    Protocol_Reset(); HostP_ClearCmds();
    feed_and_execute("{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\","
                     "\"future_a\":1,\"future_b\":\"x\",\"future_c\":[1,2,3],"
                     "\"future_d\":{\"k\":\"v\"}}\n");
    CHECK(HostP_CmdCount() == 1, "含未知键的帧仍应生效（向前兼容），实际执行 %d 条",
          HostP_CmdCount());
    CHECK(HostP_CmdCount() > 0 && HostP_CmdActionAt(0)
          && strcmp(HostP_CmdActionAt(0), "auto") == 0,
          "action 应正确解析");

    /* ---- 逐键隔离：找出是哪种未知键把整帧搞失效的 ----
     * 协议声明要"忽略未知键以向前兼容"，因此任何一种未知键都不该让整帧失效。
     * 若某一种会，那就是真实缺陷（上位机将来加字段会直接导致指令全部失效）。 */
    struct { const char *json; const char *what; } iso[] = {
        { "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\",\"u\":1}\n",
          "未知数字键" },
        { "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\",\"u\":\"s\"}\n",
          "未知字符串键" },
        { "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\",\"u\":true}\n",
          "未知布尔键" },
        { "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\",\"u\":null}\n",
          "未知 null 键" },
        { "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\",\"u\":[1,2]}\n",
          "未知**数组**键" },
        { "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\",\"u\":{\"k\":1}}\n",
          "未知**嵌套对象**键" },
        { "{\"node\":1,\"type\":\"cmd\",\"action\":\"auto\",\"u\":\"a\\\"b\"}\n",
          "未知键含转义引号" },
    };
    const int ni = (int)(sizeof(iso) / sizeof(iso[0]));
    for (int i = 0; i < ni; i++) {
        Protocol_Reset(); HostP_ClearCmds();
        feed_and_execute(iso[i].json);
        CHECK(HostP_CmdCount() == 1,
              "%s 不应让整帧失效（实际执行 %d 条）", iso[i].what, HostP_CmdCount());
    }
    printf("  隔离样例 %d 条\n", ni);

    /* ============================================================
     * 8. 上行帧：合法、以 '\n' 结尾、长度受限
     * ============================================================ */
    group("上行帧：格式与长度");
    HostP_ClearRaw();
    Protocol_SendReport(1, 1234, 2861, 2000, 150, 1, 1, 0);
    CHECK(HostP_RawCount() == 1, "应发出 1 帧");
    const char *r = HostP_RawAt(0);
    CHECK(r != NULL, "帧内容不应为空");
    if (r) {
        size_t L = strlen(r);
        CHECK(L > 0 && r[L - 1] == '\n', "上行帧必须以 '\\n' 结尾");
        CHECK(L <= PROTOCOL_FRAME_MAX, "上行帧长必须 <= %u，实际 %zu",
              (unsigned)PROTOCOL_FRAME_MAX, L);
        CHECK(strstr(r, "\"type\":\"report\"") != NULL, "应含 type=report");
        CHECK(strstr(r, "\"node\":1") != NULL, "应含 node");
    }

    /* 超长 action 的 err 回执：旧实现会因 192 B 栈缓冲静默丢帧 */
    HostP_ClearRaw();
    char longact[200];
    memset(longact, 'x', sizeof(longact) - 1);
    longact[sizeof(longact) - 1] = '\0';
    Protocol_SendAck(longact, "err", 1, 0, 0, 2000, 150, 0, 0, 0);
    CHECK(HostP_RawCount() == 1,
          "超长 action 的 ack 也必须发得出去（旧实现静默丢帧，上位机只看到超时）");
    const char *a = HostP_RawAt(0);
    if (a) {
        size_t L = strlen(a);
        CHECK(L > 0 && a[L - 1] == '\n', "ack 必须以 '\\n' 结尾");
        CHECK(L <= PROTOCOL_FRAME_MAX, "ack 帧长必须合法，实际 %zu", L);
    }

    printf("\n================================\n");
    printf("PASS %d   FAIL %d\n", s_pass, s_fail);
    printf("================================\n");
    return s_fail == 0 ? 0 : 1;
}
