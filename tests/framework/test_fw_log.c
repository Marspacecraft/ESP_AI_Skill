/**
 * @file test_fw_log.c
 * @brief `fw_log` / `fw_logp` 的边界行为。
 *
 * 这两个函数没有返回值、也没有可注入的输出口，所以这里的断言只有一类：
 * **调完之后进程还在、内存没被写坏**。真正的越界由 ASan 抓
 * （`CC="cc -fsanitize=address -g" ./build.sh run`），栈深度由真机探针抓
 * （`tests/port/idf/main/idf_main.c` 的 `run_probe()`）。
 *
 * 为什么值得单独测：`fw_vlog()` 用栈上 256 字节缓冲 + `vsnprintf()`，
 * 超长输入如果截断写错一个字节，症状是"偶尔串口乱码"或"别的变量被改"，
 * 而不是崩溃——这类错误在现场几乎查不出来。
 */
#include "framework/include/fw_app.h"
#include "tests/tinytest.h"

#include <string.h>

/**
 * @brief 调用 `fw_log`/`fw_logp` 的几种极端输入，确认都不越界。
 *
 * 入参：无。
 * 返回：无（断言式）。
 * 线程：主线程，未启动任何 App 任务。
 * 注意：不检查输出的内容——输出由 `pf_platform_log()`（平台实现）决定，
 *       框架不认识串口；这里只保证"送进去的东西不会写坏栈"。
 */
TT_CASE(null_format_is_ignored)
{
    fw_log(NULL);
    fw_logp("tag", NULL);
}

TT_CASE(overlong_tag_is_dropped)
{
    /* 标签本身超过一行缓冲（256 字节）：按设计整行丢弃，不许写出半个括号。 */
    char tag[400];

    memset(tag, 't', sizeof tag - 1u);
    tag[sizeof tag - 1u] = '\0';
    fw_logp(tag, "这行不会被打印");
}

TT_CASE(long_body_is_truncated_not_overflowed)
{
    /* 正文远超过 256 字节：`vsnprintf` 截断，缓冲最后一个字节仍是 '\0'。 */
    char body[1024];

    memset(body, 'b', sizeof body - 1u);
    body[sizeof body - 1u] = '\0';
    fw_log("正文很长：%s", body);
    fw_logp("tag", "正文很长：%s", body);
}

TT_CASE(mixed_widths_do_not_corrupt_the_stack)
{
    /* 把常见格式串一次走遍：宽度、精度、有符号、无符号、十六进制、百分号。 */
    fw_log("int=%d neg=%d u=%u hex=%08x pct=%% str=%s w=%5.2f",
           -2147483647 - 1, 42, 4294967295u, 0xDEADBEEFu, "s", 1.5);
}

int main(void)
{
    tt_set_program("test_fw_log");
    TT_RUN(null_format_is_ignored);
    TT_RUN(overlong_tag_is_dropped);
    TT_RUN(long_body_is_truncated_not_overflowed);
    TT_RUN(mixed_widths_do_not_corrupt_the_stack);
    return tt_summary();
}
