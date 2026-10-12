/*
 * cjson_test.c —— cJSON 兼容子集的 PC 端单元测试。
 *
 * 用途：在烧板前，用任意 PC C 编译器（gcc / clang / MSVC）先在宿主机验证子集解析正确性，
 *        避免直接上板才发现解析 bug。本文件含 main()，不要加入固件工程编译。
 *
 * 编译运行（Linux / macOS / WSL）：
 *   gcc cJSON.c cjson_test.c -o cjson_test && ./cjson_test
 * 编译运行（Windows，MinGW 或 MSVC）：
 *   gcc cJSON.c cjson_test.c -o cjson_test.exe && cjson_test
 *
 * 退出码 0 = 全部通过；非 0 = 至少一项失败。
 */
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("  PASS: %s\n", msg);
    } else {
        printf("  FAIL: %s\n", msg);
        g_fail++;
    }
}

int main(void)
{
    cJSON *root, *it;

    printf("[1] 合法 cmd 帧（relay_on）\n");
    root = cJSON_Parse("{\"node\":1,\"type\":\"cmd\",\"action\":\"relay_on\"}");
    check(root != NULL, "解析成功，根非空");
    if (root) {
        it = cJSON_GetObjectItemCaseSensitive(root, "type");
        check(cJSON_IsString(it) && strcmp(it->valuestring, "cmd") == 0, "type == cmd");
        it = cJSON_GetObjectItemCaseSensitive(root, "action");
        check(cJSON_IsString(it) && strcmp(it->valuestring, "relay_on") == 0, "action == relay_on");
        it = cJSON_GetObjectItemCaseSensitive(root, "node");
        check(cJSON_IsNumber(it) && it->valueint == 1, "node == 1");
        cJSON_Delete(root);
    }

    printf("[2] 带 value 的 set_th 帧\n");
    root = cJSON_Parse("{\"node\":1,\"type\":\"cmd\",\"action\":\"set_th\",\"value\":800}");
    check(root != NULL, "解析成功");
    if (root) {
        it = cJSON_GetObjectItemCaseSensitive(root, "value");
        check(cJSON_IsNumber(it) && it->valueint == 800, "value == 800");
        cJSON_Delete(root);
    }

    printf("[3] 多余空白与未知键应被忽略但解析成功\n");
    root = cJSON_Parse(" { \"type\" : \"cmd\" , \"action\" : \"auto\" , \"foo\":123 } ");
    check(root != NULL, "带空白与未知键仍解析成功");
    if (root) {
        it = cJSON_GetObjectItemCaseSensitive(root, "foo");
        check(cJSON_IsNumber(it) && it->valueint == 123, "未知键 foo==123 仍可读取");
        it = cJSON_GetObjectItemCaseSensitive(root, "missing");
        check(it == NULL, "缺失键返回 NULL");
        cJSON_Delete(root);
    }

    printf("[4] 字符串转义\n");
    root = cJSON_Parse("{\"action\":\"a\\\"b\",\"type\":\"cmd\"}");
    check(root != NULL, "含转义引号解析成功");
    if (root) {
        it = cJSON_GetObjectItemCaseSensitive(root, "action");
        check(cJSON_IsString(it) && strcmp(it->valuestring, "a\"b") == 0, "转义后 action == a\"b");
        cJSON_Delete(root);
    }

    printf("[5] 坏帧 / 尾随多余字符应返回 NULL\n");
    check(cJSON_Parse("{bad") == NULL, "未闭合对象返回 NULL");
    check(cJSON_Parse("{\"type\":\"cmd\"} x") == NULL, "尾部多余字符返回 NULL");
    check(cJSON_Parse("not json") == NULL, "纯文本返回 NULL");
    check(cJSON_Parse("") == NULL, "空串返回 NULL");

    printf("[6] 大小写敏感\n");
    root = cJSON_Parse("{\"Type\":\"cmd\",\"action\":\"relay_on\"}");
    check(root != NULL, "解析成功");
    if (root) {
        it = cJSON_GetObjectItemCaseSensitive(root, "type");   /* 小写请求 */
        check(it == NULL, "Type(大写) 不匹配 type(小写) → NULL（大小写敏感）");
        cJSON_Delete(root);
    }

    printf("\n结果：%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
