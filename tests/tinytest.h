/* tests/tinytest.h —— 全仓唯一的测试断言与用例驱动。
 *
 * 纯 C11、零依赖、单头文件。**不要 fork 出第二套**：`build.sh run` 按行解析
 * 下面的四种输出，谁改了格式，汇总结论就会静默失真。
 *
 * 输出契约（只认这四种行，一字不许改）：
 *   PASS <case>
 *   FAIL <case> -- <说明> | <file>:<line>
 *   SKIP <case> -- <原因>
 *   SUMMARY pass=N fail=M skip=K <program>
 *
 * 用法：
 *   #include "tinytest.h"          // 编译时加 -Itests
 *
 *   TT_CASE(empty_list_count_is_zero) {
 *       TT_ASSERT_INT(0, list_count(&l));
 *   }
 *
 *   int main(void)
 *   {
 *       TT_PROGRAM("test_list");
 *       TT_RUN(empty_list_count_is_zero);
 *       return tt_summary();
 *   }
 *
 * 规矩：
 *   - 断言失败 = 该用例立即结束（return），不会级联出十几条假失败；
 *   - 用例之间互不影响：每条用例跑之前都会清掉失败/跳过标记；
 *   - 每条输出都 fflush——超时被杀时，已经跑过的结果要留得住；
 *   - SKIP 不算失败（tt_summary 返回 0），但 **SKIP 不是通过**：
 *     SUMMARY 里 skip=K 是在提醒"这几条没验"。
 */
#ifndef ESP_AI_SKILL_TINYTEST_H
#define ESP_AI_SKILL_TINYTEST_H

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TT_UNUSED __attribute__((unused))
#else
#define TT_UNUSED
#endif

typedef struct {
    int pass;
    int fail;
    int skip;
    int case_failed;
    int case_skipped;
    const char *program;
} tt_state_t;

TT_UNUSED static tt_state_t tt_state = { 0, 0, 0, 0, 0, "test" };
TT_UNUSED static const char *tt_case = "?";

TT_UNUSED static void tt_set_program(const char *name)
{
    tt_state.program = name;
}

TT_UNUSED static void tt_fail(const char *file, int line, const char *msg)
{
    printf("FAIL %s -- %s | %s:%d\n", tt_case, msg, file, line);
    fflush(stdout);
    tt_state.case_failed = 1;
}

TT_UNUSED static void tt_skip(const char *reason)
{
    printf("SKIP %s -- %s\n", tt_case, reason);
    fflush(stdout);
    tt_state.case_skipped = 1;
}

TT_UNUSED static void tt_run(const char *name, void (*fn)(void))
{
    tt_state.case_failed = 0;
    tt_state.case_skipped = 0;
    tt_case = name;
    fn();

    if (tt_state.case_skipped) {
        tt_state.skip++;
        return;
    }
    if (tt_state.case_failed) {
        tt_state.fail++;
        return;
    }
    tt_state.pass++;
    printf("PASS %s\n", name);
    fflush(stdout);
}

TT_UNUSED static int tt_summary(void)
{
    printf("SUMMARY pass=%d fail=%d skip=%d %s\n",
           tt_state.pass, tt_state.fail, tt_state.skip, tt_state.program);
    fflush(stdout);
    return tt_state.fail == 0 ? 0 : 1;
}

/* 定义一个用例：函数体由调用方提供，失败时可以用 `return` 提前结束。 */
#define TT_CASE(name) TT_UNUSED static void tt_case_##name(void)

#define TT_PROGRAM(name) tt_set_program(name)

#define TT_RUN(name) tt_run(#name, tt_case_##name)

#define TT_ASSERT_INT(expect, actual)                                         \
    do {                                                                      \
        long long tt_e_ = (long long)(expect);                                \
        long long tt_a_ = (long long)(actual);                                \
        if (tt_e_ != tt_a_) {                                                 \
            char tt_buf_[160];                                                \
            snprintf(tt_buf_, sizeof tt_buf_,                                 \
                     "期望 %lld, 实际 %lld", tt_e_, tt_a_);                   \
            tt_fail(__FILE__, __LINE__, tt_buf_);                             \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TT_ASSERT_TRUE(cond)                                                  \
    do {                                                                      \
        if (!(cond)) {                                                        \
            tt_fail(__FILE__, __LINE__, "期望为真，实际为假");                 \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TT_ASSERT_FALSE(cond)                                                 \
    do {                                                                      \
        if ((cond)) {                                                         \
            tt_fail(__FILE__, __LINE__, "期望为假，实际为真");                 \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TT_ASSERT_NOT_NULL(ptr)                                               \
    do {                                                                      \
        if ((ptr) == NULL) {                                                  \
            tt_fail(__FILE__, __LINE__, "期望非 NULL，实际为 NULL");           \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TT_ASSERT_STR(expect, actual)                                         \
    do {                                                                      \
        const char *tt_e_ = (expect);                                         \
        const char *tt_a_ = (actual);                                         \
        if (tt_e_ == NULL || tt_a_ == NULL) {                                 \
            if (tt_e_ != tt_a_) {                                             \
                char tt_buf_[200];                                            \
                snprintf(tt_buf_, sizeof tt_buf_,                             \
                         "期望 \"%.120s\", 实际 \"%.120s\"",                   \
                         tt_e_ ? tt_e_ : "(null)", tt_a_ ? tt_a_ : "(null)"); \
                tt_fail(__FILE__, __LINE__, tt_buf_);                         \
                return;                                                       \
            }                                                                 \
        } else if (strcmp(tt_e_, tt_a_) != 0) {                               \
            char tt_buf_[280];                                                \
            snprintf(tt_buf_, sizeof tt_buf_,                                 \
                     "期望 \"%.120s\", 实际 \"%.120s\"", tt_e_, tt_a_);        \
            tt_fail(__FILE__, __LINE__, tt_buf_);                             \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TT_SKIP(reason)                                                       \
    do {                                                                      \
        tt_skip(reason);                                                      \
        return;                                                               \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_TINYTEST_H */
