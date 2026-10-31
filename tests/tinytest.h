/* tests/tinytest.h —— 全仓唯一的测试断言与用例驱动。
 *
 * 纯 C11、零依赖、单头文件。**不要 fork 出第二套**：输出格式被 tests/test.py
 * 按行解析，谁改了格式后面所有汇总都会静默失真。
 *
 * 输出契约（只认这四种行）：
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
 *   - 用例之间互不影响：每条用例跑之前都会清掉失败标记；
 *   - 每条输出都 fflush——超时被杀时，已经跑过的结果要留得住。
 */
#ifndef ESP_AI_SKILL_TINYTEST_H
#define ESP_AI_SKILL_TINYTEST_H

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static int tt_n_pass;
static int tt_n_fail;
static int tt_n_skip;
static int tt_case_failed;
static const char *tt_case_name = "?";
static const char *tt_program_name = "test";

#define TT_PROGRAM(p) (tt_program_name = (p))

#define TT_CASE(nm) static void nm(void)

#define TT_RUN(nm)                                                            \
    do {                                                                      \
        tt_case_name = #nm;                                                   \
        tt_case_failed = 0;                                                   \
        nm();                                                                 \
        if (!tt_case_failed) {                                                \
            tt_n_pass++;                                                      \
            printf("PASS %s\n", tt_case_name);                                \
            fflush(stdout);                                                   \
        }                                                                     \
    } while (0)

/* 本机跑不了（要真机、要某个还不存在的头）时用，不是通过。 */
#define TT_SKIP(reason)                                                       \
    do {                                                                      \
        tt_n_skip++;                                                          \
        printf("SKIP %s -- %s\n", tt_case_name, (reason));                    \
        fflush(stdout);                                                       \
        return;                                                               \
    } while (0)

#define TT_FAIL(why)                                                          \
    do {                                                                      \
        tt_case_failed = 1;                                                   \
        tt_n_fail++;                                                          \
        printf("FAIL %s -- %s | %s:%d\n", tt_case_name, (why), __FILE__,      \
               __LINE__);                                                     \
        fflush(stdout);                                                       \
        return;                                                               \
    } while (0)

#define TT_ASSERT(cond)                                                       \
    do {                                                                      \
        if (!(cond)) {                                                        \
            TT_FAIL("断言失败: " #cond);                                      \
        }                                                                     \
    } while (0)

#define TT_ASSERT_INT(expected, actual)                                       \
    do {                                                                      \
        long tt_e_ = (long)(expected);                                        \
        long tt_a_ = (long)(actual);                                          \
        if (tt_e_ != tt_a_) {                                                 \
            char tt_msg_[160];                                                \
            snprintf(tt_msg_, sizeof tt_msg_, "期望 %ld, 实际 %ld", tt_e_,    \
                     tt_a_);                                                  \
            tt_case_failed = 1;                                               \
            tt_n_fail++;                                                      \
            printf("FAIL %s -- %s | %s:%d\n", tt_case_name, tt_msg_, __FILE__, \
                   __LINE__);                                                 \
            fflush(stdout);                                                   \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TT_ASSERT_STR(expected, actual)                                       \
    do {                                                                      \
        const char *tt_e_ = (expected);                                       \
        const char *tt_a_ = (actual);                                         \
        if (tt_e_ == NULL || tt_a_ == NULL || strcmp(tt_e_, tt_a_) != 0) {    \
            char tt_msg_[256];                                                \
            snprintf(tt_msg_, sizeof tt_msg_, "期望 \"%s\", 实际 \"%s\"",     \
                     tt_e_ ? tt_e_ : "(null)", tt_a_ ? tt_a_ : "(null)");     \
            tt_case_failed = 1;                                               \
            tt_n_fail++;                                                      \
            printf("FAIL %s -- %s | %s:%d\n", tt_case_name, tt_msg_, __FILE__, \
                   __LINE__);                                                 \
            fflush(stdout);                                                   \
            return;                                                           \
        }                                                                     \
    } while (0)

#define TT_ASSERT_NOT_NULL(p)                                                 \
    do {                                                                      \
        if ((p) == NULL) {                                                    \
            TT_FAIL("不该是 NULL: " #p);                                      \
        }                                                                     \
    } while (0)

static int tt_summary(void)
{
    printf("SUMMARY pass=%d fail=%d skip=%d %s\n", tt_n_pass, tt_n_fail,
           tt_n_skip, tt_program_name);
    fflush(stdout);
    return tt_n_fail == 0 ? 0 : 1;
}

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_TINYTEST_H */
