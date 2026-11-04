/**
 * @file test_port_conformance.c
 * @brief 端口九项契约的验收套件 —— **同一份代码在 host 与真机上跑**。
 *
 * host 上它验 `platform/host-fake`，设备上它验 `tests/port/idf/components/port_c3`
 * （ESP32-C3）。两边有一条不同的实现路径，所以这份文件只允许用 `pf_ops.h`
 * 里写下的语义，不许用任何平台特有的东西。
 *
 * 为什么值得这样写：九项里的错误恰恰是"烧进去也看不出来"的那种——时钟回绕、
 * 超时变成忙等、`task_exit_current()` 之后还接着执行。它们在本文件里各自
 * 对应一条会红的用例。
 */
#include <stdint.h>
#include <string.h>

#include "tinytest.h"
#include "platform/port/pf_ops.h"

static const pf_ops_t *ops(void)
{
    return pf_platform_ops();
}

/* 跨任务可见的标志：TSan 对普通 int 会报 data race，统一走 relaxed 原子。 */
static int flag_get(const int *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static void flag_set(int *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* 静态自洽                                                           */
/* ------------------------------------------------------------------ */

TT_CASE(table_is_conforming)
{
    const pf_ops_t *t = ops();

    TT_ASSERT_NOT_NULL(t);
    TT_ASSERT_INT((int)PF_OPS_VERSION, (int)t->version);
    TT_ASSERT_INT(0, (int)(t->caps() & PF_CAP_RESERVED_MASK));
    TT_ASSERT_NOT_NULL((void *)t->mem_alloc);
    TT_ASSERT_NOT_NULL((void *)t->mem_free);
    TT_ASSERT_NOT_NULL((void *)t->clock_ms);
    TT_ASSERT_NOT_NULL((void *)t->sleep_ms);
    TT_ASSERT_NOT_NULL((void *)t->mutex_create);
    TT_ASSERT_NOT_NULL((void *)t->mutex_take);
    TT_ASSERT_NOT_NULL((void *)t->mutex_give);
    TT_ASSERT_NOT_NULL((void *)t->task_create);
    TT_ASSERT_NOT_NULL((void *)t->task_exit_current);
}

/* ------------------------------------------------------------------ */
/* 内存                                                               */
/* ------------------------------------------------------------------ */

TT_CASE(alloc_zero_is_null)
{
    TT_ASSERT_TRUE(ops()->mem_alloc(0u) == NULL);
}

TT_CASE(allocated_block_is_writable)
{
    unsigned char *p = (unsigned char *)ops()->mem_alloc(64u);

    TT_ASSERT_NOT_NULL(p);
    if (p == NULL) {
        return;
    }
    for (uint32_t i = 0u; i < 64u; i++) {
        p[i] = (unsigned char)(i * 3u + 1u);
    }
    for (uint32_t i = 0u; i < 64u; i++) {
        TT_ASSERT_INT((int)((i * 3u + 1u) & 0xffu), (int)p[i]);
    }
    ops()->mem_free(p);
}

TT_CASE(free_null_is_a_noop)
{
    ops()->mem_free(NULL); /* 不许崩：契约要求它是无操作 */
    TT_ASSERT_TRUE(1);
}

/* ------------------------------------------------------------------ */
/* 时钟与延时                                                         */
/* ------------------------------------------------------------------ */

TT_CASE(clock_is_monotonic)
{
    uint32_t start = ops()->clock_ms();

    for (int i = 0; i < 20; i++) {
        ops()->sleep_ms(1u);
    }
    /* 差值比较，回绕也成立；48ms 的等待不该超过 1 秒。 */
    TT_ASSERT_TRUE((uint32_t)(ops()->clock_ms() - start) < 1000u);
}

TT_CASE(sleep_25ms_takes_at_least_20ms)
{
    uint32_t start = ops()->clock_ms();
    uint32_t elapsed;

    ops()->sleep_ms(25u);
    elapsed = (uint32_t)(ops()->clock_ms() - start);

    TT_ASSERT_TRUE(elapsed >= 20u); /* 睡少了 = 忙等或截断 */
    TT_ASSERT_TRUE(elapsed < 500u); /* 睡多了 = tick 换算错了一个量级 */
}

TT_CASE(sleep_zero_returns_quickly)
{
    uint32_t start = ops()->clock_ms();
    uint32_t elapsed;

    ops()->sleep_ms(0u);
    elapsed = (uint32_t)(ops()->clock_ms() - start);

    TT_ASSERT_TRUE(elapsed < 50u);
}

/* ------------------------------------------------------------------ */
/* 互斥                                                               */
/* ------------------------------------------------------------------ */

TT_CASE(mutex_uncontended_round_trip)
{
    pf_mutex_t *m = ops()->mutex_create();

    TT_ASSERT_NOT_NULL(m);
    if (m == NULL) {
        return;
    }
    TT_ASSERT_INT(0, ops()->mutex_take(m, 100u));
    ops()->mutex_give(m);
    ops()->mutex_give(m); /* 多发一次不该崩，也不该永久破坏这把锁 */
    TT_ASSERT_INT(0, ops()->mutex_take(m, 100u));
    ops()->mutex_give(m);
}

TT_CASE(mutex_double_take_fails)
{
    pf_mutex_t *m = ops()->mutex_create();

    TT_ASSERT_NOT_NULL(m);
    if (m == NULL) {
        return;
    }
    TT_ASSERT_INT(0, ops()->mutex_take(m, 100u));
    TT_ASSERT_INT(-1, ops()->mutex_take(m, 0u)); /* 非递归：同一任务再取必失败 */
    ops()->mutex_give(m);
    TT_ASSERT_INT(0, ops()->mutex_take(m, 100u));
    ops()->mutex_give(m);
}

/* ------------------------------------------------------------------ */
/* 任务                                                               */
/* ------------------------------------------------------------------ */

static int g_ran;
static int g_after_exit;
static int g_take_result;

static void task_marks_ran(void *arg)
{
    flag_set((int *)arg, 1);
}

static void task_exits_without_returning(void *arg)
{
    (void)arg;
    flag_set(&g_after_exit, 0);
    ops()->task_exit_current();
    /* 这一行**永远不该执行**：平台承诺不返回。 */
    flag_set(&g_after_exit, 1);
}

static void task_takes_after_delay(void *arg)
{
    pf_mutex_t *m = (pf_mutex_t *)arg;

    ops()->sleep_ms(20u);
    flag_set(&g_take_result, ops()->mutex_take(m, 5u)); /* 相对超时：必失败 */
}

/* host 上没有"等一个任务跑完"的平台接口，只能轮询 + 超时。 */
static int wait_flag(const int *p, int want, uint32_t timeout_ms)
{
    uint32_t start = ops()->clock_ms();

    while ((uint32_t)(ops()->clock_ms() - start) < timeout_ms) {
        if (flag_get(p) == want) {
            return 1;
        }
        ops()->sleep_ms(1u);
    }
    return flag_get(p) == want;
}

TT_CASE(task_rejects_small_stack)
{
    pf_task_desc_t d = {"small", task_marks_ran, &g_ran, 512u};

    TT_ASSERT_TRUE(ops()->task_create(&d) == NULL);
}

TT_CASE(task_rejects_null_entry)
{
    pf_task_desc_t d = {"noentry", NULL, NULL, (uint32_t)PF_TASK_MIN_STACK};

    TT_ASSERT_TRUE(ops()->task_create(&d) == NULL);
}

TT_CASE(task_runs_its_entry)
{
    pf_task_desc_t d = {"runner", task_marks_ran, &g_ran, 4096u};

    flag_set(&g_ran, 0);
    TT_ASSERT_NOT_NULL(ops()->task_create(&d));
    TT_ASSERT_TRUE(wait_flag(&g_ran, 1, 2000u));
}

TT_CASE(task_exit_current_does_not_return)
{
    pf_task_desc_t d = {"exiter", task_exits_without_returning, NULL, 4096u};

    flag_set(&g_after_exit, 0);
    TT_ASSERT_NOT_NULL(ops()->task_create(&d));
    /* 给它足够时间跑完并"返回"——如果真返回了，g_after_exit 会变成 1。 */
    ops()->sleep_ms(100u);
    TT_ASSERT_INT(0, flag_get(&g_after_exit));
}

TT_CASE(mutex_timeout_is_relative)
{
    pf_mutex_t *m = ops()->mutex_create();
    pf_task_desc_t d;

    TT_ASSERT_NOT_NULL(m);
    if (m == NULL) {
        return;
    }
    flag_set(&g_take_result, 99);
    d.name = "contender";
    d.entry = task_takes_after_delay;
    d.arg = m;
    d.stack_bytes = 4096u;

    TT_ASSERT_INT(0, ops()->mutex_take(m, 1000u)); /* 主任务先拿着 */
    TT_ASSERT_NOT_NULL(ops()->task_create(&d));
    TT_ASSERT_TRUE(wait_flag(&g_take_result, -1, 2000u)); /* 等到 worker 超时 */
    ops()->mutex_give(m);
}

int main(void)
{
    TT_PROGRAM("test_port_conformance");

    TT_RUN(table_is_conforming);
    TT_RUN(alloc_zero_is_null);
    TT_RUN(allocated_block_is_writable);
    TT_RUN(free_null_is_a_noop);
    TT_RUN(clock_is_monotonic);
    TT_RUN(sleep_25ms_takes_at_least_20ms);
    TT_RUN(sleep_zero_returns_quickly);
    TT_RUN(mutex_uncontended_round_trip);
    TT_RUN(mutex_double_take_fails);
    TT_RUN(task_rejects_small_stack);
    TT_RUN(task_rejects_null_entry);
    TT_RUN(task_runs_its_entry);
    TT_RUN(task_exit_current_does_not_return);
    TT_RUN(mutex_timeout_is_relative);

    return tt_summary();
}
