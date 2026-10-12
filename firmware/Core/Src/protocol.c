/**
 * @file protocol.c
 * @brief 协议层：下行帧断帧/解析、上行 report/ack/evt 组包。
 *
 * 设计要点（Tech_Design §通信协议 / §接收行缓冲）：
 *  - 帧格式：TCP + '\n' 分隔 JSON，UTF-8，字段全小写；
 *  - 透传模式下数据不带 +IPD 前缀，必须自行行缓冲：环形缓冲 + '\n' 断帧 +
 *    单帧上限 + 连续无换行丢弃保护；
 *  - 上行组包用 snprintf（确定性内存，无堆分配），下行解析用 cJSON；
 *  - 忽略未知键 / 未知 type / 坏帧（向前兼容）；
 *  - 动作词汇的单一事实源是 tools/protocol_schema.py，本文件的分支必须与之保持一致，
 *    并由 tools/contract_test.py 守护（get_state 是合法动作，query 不是）。
 */

#include "protocol.h"
#include "esp8266.h"
#include "control.h"
#include "main.h"
#include "cJSON.h"

/* ===================== 下行行缓冲 ===================== */

#define RX_RING_SIZE      256U    /* 环形缓冲大小 */

/* 单帧上限的**唯一事实源在 protocol.h**（main.c 的取帧缓冲 cmd_buf 也用同一个宏）。
 * 历史上这里另立了 RX_FRAME_MAX / RX_LINE_BUF 两份 200，加上 protocol.h 的
 * PROTOCOL_FRAME_MAX 共三份互不关联 —— 改一处忘另一处就会静默截断：
 * 若把 PROTOCOL_FRAME_MAX 调小（如 100），main.c 的 cmd_buf 变成 100，
 * 而本文件仍按 200 产出帧，取出时被截断成非法 JSON -> 指令静默丢失且无 ack。
 * 现在统一引用 protocol.h 的宏，消除这类漂移。 */
#define RX_LINE_BUF       PROTOCOL_FRAME_MAX

static uint8_t  s_rx_ring[RX_RING_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static char     s_line[RX_LINE_BUF];
static uint16_t s_line_len;

/* 清空上一轮的链路残留（行装配器 + 环形缓冲）。
 * 断链若正好停在一帧 JSON 中途，s_line 里会留半帧且 s_line_len > 0；
 * 而 s_line_len 只在"成帧后"和"溢出时"被清零，于是重连后新字节被**追加到旧半帧之后**,
 * 第一条指令必然解析失败、静默丢弃（上位机只会看到 ack 超时）。
 * esp8266.c 侧的 AT 状态清理很完整，但本文件的行装配器是 static、
 * 此前没有任何跨模块复位入口 —— 形成"驱动清干净了、协议层还留着脏数据"的断层。
 * 调用时机：每条新链路开始之前（开机、每次链路丢失后）。 */
void Protocol_Reset(void)
{
    /* 关键区：head 由 USART3 ISR 写、tail 由主循环写，必须一起归零，
     * 否则中途会出现 head/tail 不一致的瞬间。只屏蔽极短时间，不影响 SysTick 精度。 */
    __disable_irq();
    s_rx_head = 0U;
    s_rx_tail = 0U;
    __enable_irq();

    s_line_len = 0U;
    s_line[0]  = '\0';
}

void Protocol_RxByte(uint8_t b)
{
    uint16_t next = (uint16_t)((s_rx_head + 1U) % RX_RING_SIZE);

    if (next == s_rx_tail) {
        return;                            /* 缓冲满：丢最新字节，保护已收帧 */
    }
    s_rx_ring[s_rx_head] = b;
    s_rx_head = next;
}

static int ring_pop(void)
{
    uint8_t b;

    if (s_rx_head == s_rx_tail) {
        return -1;
    }
    b = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) % RX_RING_SIZE);
    return (int)b;
}

bool Protocol_PopCommand(char *out, size_t cap)
{
    int c;

    if (out == NULL || cap == 0U) {
        return false;
    }

    while ((c = ring_pop()) >= 0) {
        if (c == '\n' || c == '\r') {
            if (s_line_len == 0U) {
                continue;                  /* 空行 / \r\n 的 \n：忽略 */
            }
            s_line[s_line_len] = '\0';
            strncpy(out, s_line, cap - 1U);
            out[cap - 1U] = '\0';
            s_line_len = 0U;
            return true;
        }
        if (s_line_len < (RX_LINE_BUF - 1U)) {
            s_line[s_line_len++] = (char)c;
        } else {
            s_line_len = 0U;               /* 超长无换行：丢弃并复位，防缓冲被撑爆 */
        }
    }
    return false;
}

/* ===================== 下行解析 ===================== */

void Protocol_ParseAndExecute(char *frame)
{
    cJSON *root;
    cJSON *jnode;
    cJSON *jtype;
    cJSON *jact;
    cJSON *jval;
    int32_t value = -1;

    if (frame == NULL) {
        return;
    }

    root = cJSON_Parse(frame);
    if (root == NULL) {
        return;                            /* 非 JSON：忽略 */
    }

    jtype = cJSON_GetObjectItemCaseSensitive(root, "type");
    jact  = cJSON_GetObjectItemCaseSensitive(root, "action");

    /* 只处理下行 cmd；report/ack/evt 与未知 type 忽略（向前兼容） */
    if (cJSON_IsString(jtype) && (strcmp(jtype->valuestring, "cmd") == 0)
        && cJSON_IsString(jact)) {

        /* ★ 节点过滤（原实现有两个漏洞）：
         *  1) 原来写的是 `((uint8_t)jnode->valueint != NODE_ID)`，先把值**截成 8 位**再比,
         *     于是 {"node":257} 会被节点 1 **接受**（257 & 0xFF == 1）—— 类型宽度不闭环；
         *  2) 若 node 缺失或不是数字，`cJSON_IsNumber(NULL)` 为假，整个 if 被跳过,
         *     帧被**无条件执行**，与该处注释"不是发给我的帧不执行"完全相反。
         * 现在：先要求 node 必须存在且为数字，再用**完整 int32** 比较（不截断），
         * 任何缺失或不匹配一律丢弃。 */
        jnode = cJSON_GetObjectItemCaseSensitive(root, "node");
        if (!cJSON_IsNumber(jnode)) {
            cJSON_Delete(root);            /* 缺少 node：拒绝，不猜 */
            return;
        }
        if ((int32_t)jnode->valueint != (int32_t)NODE_ID) {
            cJSON_Delete(root);            /* 多板场景：不是发给我的帧不执行 */
            return;
        }

        jval = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (cJSON_IsNumber(jval)) {
            value = (int32_t)jval->valueint;
        }
        Control_OnCommand(jact->valuestring, value);
    }

    cJSON_Delete(root);
}

/* ===================== 上行组包 ===================== */

/* 发送出口：透传未建立时丢弃，不影响本地闭环 */
static void send_frame(const char *json, uint16_t len)
{
    char buf[RX_LINE_BUF + 2U];

    if (len == 0U || len > RX_LINE_BUF) {
        return;
    }
    memcpy(buf, json, len);
    buf[len] = '\n';
    Esp8266_SendRaw(buf, (uint16_t)(len + 1U));
}

void Protocol_SendReport(uint8_t node, uint16_t adc, uint16_t light, uint16_t th, uint16_t hys,
                         uint8_t relay, uint8_t buzz, uint8_t mode)
{
    char j[160];
    int n = snprintf(j, sizeof(j),
        "{\"node\":%u,\"type\":\"report\",\"adc\":%u,\"light\":%u,\"th\":%u,"
        "\"hys\":%u,\"relay\":%u,\"buzz\":%u,\"mode\":%u}",
        (unsigned)node, (unsigned)adc, (unsigned)light, (unsigned)th,
        (unsigned)hys, (unsigned)relay, (unsigned)buzz, (unsigned)mode);
    if (n > 0 && n < (int)sizeof(j)) {
        send_frame(j, (uint16_t)n);
    }
}

void Protocol_SendAck(const char *action, const char *result, uint8_t node,
                      uint16_t adc, uint16_t light, uint16_t th, uint16_t hys,
                      uint8_t relay, uint8_t buzz, uint8_t mode)
{
    /* ★ action 来自下行 JSON 的**任意**字符串（最长可达 RX_LINE_BUF-1 ≈ 199 字符），
     * 直接 %s 回显会撑爆下面的 j[192] —— 于是"未知动作一律回 err"这个承诺
     * 在 len(action) ≥ 70 时**静默失约**：帧在这里被丢弃，上位机只看到 ack 超时，
     * 完全不知道指令被拒了。修复：先把 action 截断到一定能装下的长度，
     * 保证**任何**输入都能回出一帧合法 JSON。
     * 已知动作名最长 10 字符（alarm_test），因此正常路径永不触发截断；
     * 只有"未知动作"的错误回执会显示被截断的名字 —— 那本来就是诊断信息。 */
    char act[48];
    (void)snprintf(act, sizeof(act), "%s", (action != NULL) ? action : "");

    char j[192];
    int n = snprintf(j, sizeof(j),
        "{\"node\":%u,\"type\":\"ack\",\"action\":\"%s\",\"result\":\"%s\","
        "\"adc\":%u,\"light\":%u,\"th\":%u,\"hys\":%u,\"relay\":%u,\"buzz\":%u,\"mode\":%u}",
        (unsigned)node, act, (result != NULL ? result : ""),
        (unsigned)adc, (unsigned)light, (unsigned)th, (unsigned)hys,
        (unsigned)relay, (unsigned)buzz, (unsigned)mode);
    if (n > 0 && n < (int)sizeof(j)) {
        send_frame(j, (uint16_t)n);
    }
}

void Protocol_SendEvent(const char *event, uint8_t node, uint16_t adc, uint16_t light)
{
    char j[96];
    int n = snprintf(j, sizeof(j),
        "{\"node\":%u,\"type\":\"evt\",\"event\":\"%s\",\"adc\":%u,\"light\":%u}",
        (unsigned)node, (event != NULL ? event : ""), (unsigned)adc, (unsigned)light);
    if (n > 0 && n < (int)sizeof(j)) {
        send_frame(j, (uint16_t)n);
    }
}
