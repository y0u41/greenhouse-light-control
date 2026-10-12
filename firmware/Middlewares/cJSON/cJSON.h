/*
 * cJSON.h —— 与官方 cJSON (https://github.com/DaveGamble/cJSON, MIT) 兼容的轻量子集。
 *
 * 本文件只实现「智能温室」协议层实际用到的解析 API（扁平 JSON 对象 + 字符串 / 数字 / 字面量），
 * 结构体字段布局（next/prev/child/type/valuestring/valueint/valuedouble/string）与官方完全一致，
 * 因此可直接替换为本仓库所需的解析功能，无需改动 protocol.c。
 *
 * 为什么内置子集而非引入完整 cJSON：
 *   消除上板编译的「外部依赖缺口」，保持仓库自包含、可提交、可审计。
 *   完整 cJSON 还会引入打印 / 构造 / 数组 / 嵌套等当前协议用不到的分支（且对堆压力更大）。
 *
 * 如何升级到完整功能：
 *   把本目录 cJSON.c / cJSON.h 整体替换为官方 cJSON 的对应文件即可，protocol.c 无需改动
 *   （官方 cJSON 的 API 与字段是本子集的超集）。
 */
#ifndef CJSON_H_MIN
#define CJSON_H_MIN

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int cJSON_bool;

/* JSON 类型位（与官方 cJSON 数值一致，便于用位与(&)判断） */
#define cJSON_Invalid    (0)
#define cJSON_False      (1 << 0)
#define cJSON_True       (1 << 1)
#define cJSON_NULL       (1 << 2)
#define cJSON_Number     (1 << 3)
#define cJSON_String     (1 << 4)
#define cJSON_Array      (1 << 5)
#define cJSON_Object     (1 << 6)
#define cJSON_Raw        (1 << 7)

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int          type;
    char        *valuestring;   /* 字符串值（解析器分配，cJSON_Delete 时释放） */
    int          valueint;      /* 整数值（数字节点） */
    double       valuedouble;   /* 浮点值（数字节点） */
    char        *string;        /* 成员键名（解析器分配，cJSON_Delete 时释放） */
} cJSON;

/* 解析顶层 value，返回根节点（通常为 cJSON_Object）。失败返回 NULL。
   调用方负责 cJSON_Delete(root)。 */
cJSON *cJSON_Parse(const char *value);

/* 大小写敏感地查找 object 的子项（按键名 string 匹配）。找不到返回 NULL。 */
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name);

cJSON_bool cJSON_IsString(const cJSON *item);
cJSON_bool cJSON_IsNumber(const cJSON *item);

void cJSON_Delete(cJSON *item);

#ifdef __cplusplus
}
#endif

#endif /* CJSON_H_MIN */
