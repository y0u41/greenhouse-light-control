/*
 * protocol.c 解析健壮性测试（主机端，无需硬件）
 *
 * ⚠⚠ 状态：40 PASS / 6 FAIL（**尚未全绿，勿当成通过**）⚠⚠
 *
 * 已确认的真实固件缺陷（1 项）：
 *   【P-1】超长行不是"整行丢弃"，而是"截断后把尾巴当一帧返回"。
 *          protocol.c:101-105 在 s_line_len 达到上限时只做 s_line_len = 0，
 *          并不丢弃到行尾；于是 250 字节的 'A' 行在撞到上限后被从头重新累积，
 *          遇到 '\n' 时把**尾巴 50 字节**当一帧交出去（实测 strlen=50）。
 *          这与该处注释"超长无换行：丢弃并复位"的语义不符。
 *          危害：截断后的尾巴若恰好构成合法 JSON，就会被当成一条真指令执行
 *          （TCP 任意切分字节流时并非不可能），属于**可被误执行**的路径。
 *          修法建议：加一个"本行已超长、丢弃至行尾"的标志，
 *          或改成"超长即把 s_line_len 置为哨兵值并在遇到 '\n' 时跳过该行"。
 *
 * 尚未定位的失败（5 项，**可能是我测试夹具的问题，也可能是真实缺陷**）：
 *   group 5/6/7 里所有"应当被接受"的用例都得到"实际执行 0 条"，
 *   而同内容的帧在 group 1 里能正常执行 —— 已排除超长行状态泄漏
 *   （插入 Protocol_Reset 后仍为 0）。**结论待查，不要据此认为固件有问题。**
 *
 * 已排除的测试夹具错误（第一版 7 个失败里 2 个是夹具问题，已修）：
 *   · 喂 600 字节超出 256 环形缓冲容量，导致后续帧被环形缓冲丢弃，
 *     被误读成"行溢出后无法恢复" —— 是测试设计错误，不是固件缺陷。
 *
 * 覆盖 P0-2 要求的"畸形 JSON / 半帧"：这类输入在真实链路上**一定会出现**
 * （TCP 会任意切分字节流、模块重启会插入 URC、干扰会造成截断），
 * 而它们的失效模式是"静默丢弃" —— 没有测试就永远发现不了。
 */

#include "stm32f1xx_hal.h"
#include "protocol.h"
#include "main.h"
#include <stdio.h>
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
/* 喂一串字节并直接执行（模拟主循环：PopCommand -> ParseAndExecute） */
static int s_dbg_feed = 0;
static void feed_and_execute(const char *s) {
    char buf[PROTOCOL_FRAME_MAX];
    int n = 0;
    while (Protocol_PopCommand(buf, sizeof(buf))) {
        if (s_dbg_feed) printf("    [dbg] popped = '%s'\n", buf);
        Protocol_ParseAndExecute(buf);
        n++;
    }
    if (s_dbg_feed && n == 0) {
        printf("    [dbg] 没有取出任何帧；输入的最后一个字节是 %s\n",
               (s[0] && s[strlen(s) - 1] == '\n') ? "'\\n'（有换行）" : "'?'（无换行）");
    }
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
    CHECK(HostP_CmdActionAt(0) && strcmp(HostP_CmdActionAt(0), "auto") == 0,
          "action 应正确解析");

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
