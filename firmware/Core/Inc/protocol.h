#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 单帧上限（与 protocol.c 行缓冲一致）；main.c 的取帧缓冲按此大小分配 */
#define PROTOCOL_FRAME_MAX   200U

/* ===== 下行（Qt → 节点）===== */
/* 主循环调用：取出一条完整帧（行缓冲 + '\n' 断帧） */
bool Protocol_PopCommand(char *out, size_t cap);
/* 解析并执行一条下行帧；非 JSON / 未知 type 一律忽略（向前兼容） */
void Protocol_ParseAndExecute(char *frame);

/* ===== 上行（节点 → Qt）===== */
void Protocol_SendReport(uint8_t node, uint16_t adc, uint16_t light, uint16_t th, uint16_t hys,
                         uint8_t relay, uint8_t buzz, uint8_t mode);
void Protocol_SendAck(const char *action, const char *result, uint8_t node,
                      uint16_t adc, uint16_t light, uint16_t th, uint16_t hys,
                      uint8_t relay, uint8_t buzz, uint8_t mode);
void Protocol_SendEvent(const char *event, uint8_t node, uint16_t adc, uint16_t light);

/* ===== 接收入口 =====
 * 由 esp8266 在透传模式下调用：AT+CIPMODE=1 后收到的数据不带 +IPD 前缀，
 * 必须由本模块做环形缓冲 + '\n' 断帧，并限制单帧与缓冲上限。 */
void Protocol_RxByte(uint8_t b);

/* 清空上一轮的链路残留（行装配器 + 环形缓冲）。
 * **每条新链路开始之前必须调用**（开机、以及每次链路丢失后）：
 * 断链若停在一帧 JSON 中途，s_line 会留半帧，重连后新字节被追加到旧半帧之后，
 * 导致重连后的第一条指令必然解析失败并被静默丢弃（连 err ack 都没有）。
 * 只清接收侧缓冲，不触碰任何业务状态。 */
void Protocol_Reset(void);

#endif /* PROTOCOL_H */
