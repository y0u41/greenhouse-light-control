/*
 * cJSON.c —— 兼容子集实现。详见 cJSON.h 头部说明。
 *
 * 支持的语法（仅覆盖协议帧所需）：
 *   value  := object | string | number | true | false | null
 *   object := '{' (pair (',' pair)*)? '}'
 *   pair   := string ':' value
 *   string := '"' char* '"'   （支持 \" \\ \/ \n \t \r \b \f，\uXXXX 简化为单个占位 '?'）
 *   number := '-'? digits ('.' digits)? (('e'|'E') ('+'|'-')? digits)?
 *
 * 设计取舍（与协议层契约一致）：
 *   - 顶层只接受「单一 value + 尾随空白」；存在多余非空白字符视为坏帧，返回 NULL，
 *     符合 protocol.c 「忽略坏帧 / 向前兼容」原则；
 *   - 所有节点用 calloc/malloc 分配，cJSON_Delete 递归释放；
 *   - STM32 上需保证堆（_sbrk）足够：解析单帧峰值 < 1KB，建议工程 Heap_Size 调到 0x800 以上
 *     （见 firmware/CUBEMX_SETUP.md）。
 */
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *p;     /* 当前读指针 */
    const char *end;   /* 缓冲区末尾（不含） */
    int         err;   /* 解析错误标志 */
} parser;

/* ------------------------- 基础工具 ------------------------- */

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ps->p++;
        } else {
            break;
        }
    }
}

static cJSON *new_node(int type)
{
    cJSON *n = (cJSON *)calloc(1, sizeof(cJSON));
    if (n) {
        n->type = type;
    }
    return n;
}

/* ------------------------- 字符串 ------------------------- */

/* 调用前须保证 *ps->p == '"'。返回字符串节点（valuestring 已分配），失败返回 NULL 且不泄漏。 */
static cJSON *parse_string(parser *ps)
{
    ps->p++;                                   /* 跳过起始引号 */

    /* 第一遍：确定未转义后的长度，并校验闭合 */
    const char *q = ps->p;
    size_t len = 0;
    while (q < ps->end && *q != '"') {
        if (*q == '\\') {
            q++;                               /* 转义符，后随一个字符（\u 占 4 hex，按 1 输出字符计） */
        }
        q++;
        len++;
    }
    if (q >= ps->end) {                        /* 未闭合 */
        ps->err = 1;
        return NULL;
    }

    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        ps->err = 1;
        return NULL;
    }

    /* 第二遍：拷贝并解转义 */
    size_t i = 0;
    q = ps->p;
    while (q < ps->end && *q != '"') {
        if (*q == '\\') {
            q++;
            if (q >= ps->end) {
                break;
            }
            char c = *q;
            switch (c) {
                case '"':  buf[i++] = '"';  break;
                case '\\': buf[i++] = '\\'; break;
                case '/':  buf[i++] = '/';  break;
                case 'n':  buf[i++] = '\n'; break;
                case 't':  buf[i++] = '\t'; break;
                case 'r':  buf[i++] = '\r'; break;
                case 'b':  buf[i++] = '\b'; break;
                case 'f':  buf[i++] = '\f'; break;
                case 'u':  q += 4; buf[i++] = '?'; break;   /* 简化：非 ASCII 占位 */
                default:   buf[i++] = c;   break;
            }
            q++;
        } else {
            buf[i++] = *q;
            q++;
        }
    }
    buf[i] = '\0';
    ps->p = q + 1;                             /* 跳过结束引号 */

    cJSON *node = new_node(cJSON_String);
    if (!node) {
        free(buf);
        ps->err = 1;
        return NULL;
    }
    node->valuestring = buf;
    return node;
}

/* ------------------------- 数字 ------------------------- */

static cJSON *parse_number(parser *ps)
{
    const char *start = ps->p;

    if (ps->p < ps->end && *ps->p == '-') {
        ps->p++;
    }
    while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
        ps->p++;
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
            ps->p++;
        }
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) {
            ps->p++;
        }
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
            ps->p++;
        }
    }

    size_t len = (size_t)(ps->p - start);
    if (len == 0) {
        ps->err = 1;
        return NULL;
    }

    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        ps->err = 1;
        return NULL;
    }
    memcpy(tmp, start, len);
    tmp[len] = '\0';

    cJSON *node = new_node(cJSON_Number);
    if (!node) {
        free(tmp);
        ps->err = 1;
        return NULL;
    }
    node->valuedouble = strtod(tmp, NULL);
    node->valueint = (int)node->valuedouble;
    free(tmp);
    return node;
}

/* ------------------------- 字面量 ------------------------- */

static cJSON *parse_literal(parser *ps, const char *lit, int type)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || strncmp(ps->p, lit, n) != 0) {
        ps->err = 1;
        return NULL;
    }
    ps->p += n;
    return new_node(type);
}

/* ------------------------- 对象 / value ------------------------- */

static cJSON *parse_value(parser *ps);          /* 前向声明 */

/**
 * @brief 解析 JSON 数组 [v1, v2, ...]
 *
 * ★ 为什么补这个函数（修复 P-1）★
 * 本文件原先**没有数组解析**：parse_value() 只处理 '{'，遇到 '[' 直接落到
 * "非法字符"分支置 ps->err，导致 cJSON_Parse 返回 NULL。
 * 后果不是"数组被忽略"，而是**含数组的整帧解析失败** —— 而
 * protocol.c:126-128 对解析失败是静默 return（连 err ack 都不发），
 * 于是表现为"指令没反应、上位机 ack 超时"，且没有任何可观测错误。
 *
 * 这与协议声明的"忽略未知键以向前兼容"直接冲突：上位机只要新增**任何**
 * 数组字段（批处理列表、历史点、多阈值…），该帧就整条失效。
 * 由 tests/host/test_protocol.c 的逐键隔离用例定位（7 种未知键里唯独数组致命）。
 *
 * 实现与 parse_object 对称：数组元素同样挂在 child/next/prev 链上，
 * 因此现有的 cJSON_Delete()（:351）无需改动即可递归释放数组元素。
 */
static cJSON *parse_array(parser *ps)
{
    /* 调用前须保证 *ps->p == '[' */
    ps->p++;

    cJSON *arr = new_node(cJSON_Array);
    if (!arr) {
        ps->err = 1;
        return NULL;
    }

    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {     /* 空数组 [] */
        ps->p++;
        return arr;
    }

    cJSON *prev = NULL;
    while (1) {
        skip_ws(ps);
        if (ps->p >= ps->end) {                 /* 未闭合 */
            ps->err = 1;
            break;
        }
        /* 与对象不同：数组元素**没有键名**，直接是 value。
         * 嵌套数组/对象由 parse_value 递归处理。 */
        cJSON *val = parse_value(ps);
        if (!val) {
            ps->err = 1;
            break;
        }

        if (arr->child == NULL) {
            arr->child = val;
        } else {
            prev->next = val;
        }
        val->prev = prev;
        prev = val;

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            break;
        }
        ps->err = 1;
        break;
    }
    return arr;
}

static cJSON *parse_object(parser *ps)
{
    /* 调用前须保证 *ps->p == '{' */
    ps->p++;

    cJSON *obj = new_node(cJSON_Object);
    if (!obj) {
        ps->err = 1;
        return NULL;
    }

    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {     /* 空对象 */
        ps->p++;
        return obj;
    }

    cJSON *prev = NULL;
    while (1) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            ps->err = 1;
            break;
        }

        cJSON *key = parse_string(ps);
        if (!key) {
            ps->err = 1;
            break;
        }

        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            free(key->valuestring);
            free(key);
            ps->err = 1;
            break;
        }
        ps->p++;                                /* 跳过 ':' */
        skip_ws(ps);

        cJSON *val = parse_value(ps);
        if (!val) {
            free(key->valuestring);
            free(key);
            ps->err = 1;
            break;
        }

        /* 键名缓冲转移给 val->string，释放 key 节点壳（缓冲不释放） */
        val->string = key->valuestring;
        free(key);

        if (obj->child == NULL) {
            obj->child = val;
        } else {
            prev->next = val;
        }
        val->prev = prev;
        prev = val;

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            break;
        }
        ps->err = 1;
        break;
    }
    return obj;
}

static cJSON *parse_value(parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        ps->err = 1;
        return NULL;
    }
    char c = *ps->p;
    if (c == '{') {
        return parse_object(ps);
    }
    if (c == '[') {                             /* 数组（P-1 修复前缺失，见 parse_array 注释） */
        return parse_array(ps);
    }
    if (c == '"') {
        return parse_string(ps);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return parse_number(ps);
    }
    if (c == 't') {
        return parse_literal(ps, "true", cJSON_True);
    }
    if (c == 'f') {
        return parse_literal(ps, "false", cJSON_False);
    }
    if (c == 'n') {
        return parse_literal(ps, "null", cJSON_NULL);
    }
    ps->err = 1;
    return NULL;
}

/* ------------------------- 公共 API ------------------------- */

cJSON *cJSON_Parse(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    parser ps;
    ps.p = value;
    ps.end = value + strlen(value);
    ps.err = 0;

    cJSON *root = parse_value(&ps);
    if (ps.err || root == NULL) {
        if (root) {
            cJSON_Delete(root);
        }
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {                       /* 尾部存在非空白字符：坏帧 */
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name)
{
    if (object == NULL || object->child == NULL || name == NULL) {
        return NULL;
    }
    cJSON *c = object->child;
    while (c) {
        if (c->string != NULL && strcmp(c->string, name) == 0) {
            return c;
        }
        c = c->next;
    }
    return NULL;
}

cJSON_bool cJSON_IsString(const cJSON *item)
{
    return (item != NULL && (item->type & cJSON_String)) ? 1 : 0;
}

cJSON_bool cJSON_IsNumber(const cJSON *item)
{
    return (item != NULL && (item->type & cJSON_Number)) ? 1 : 0;
}

void cJSON_Delete(cJSON *item)
{
    cJSON *next;
    while (item) {
        next = item->next;
        if (item->child) {
            cJSON_Delete(item->child);
        }
        if (item->valuestring) {
            free(item->valuestring);
        }
        if (item->string) {
            free(item->string);
        }
        free(item);
        item = next;
    }
}
