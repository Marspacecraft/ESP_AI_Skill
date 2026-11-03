/* tests/platform/test_pf_conformance.c —— 端口契约语义验收（步骤 1.4）。
 *
 * 这份用例不属于某块板，它验收的是"任何一张 pf_ops_t 都必须做到的事"。
 * 换一块新板时，把这块板的表喂进来跑同一份用例，就是接板的第一道关。
 *
 * 分两段：
 *   A. 静态自洽（版本号、保留位、九项不许缺）——将来会由 fw_init() 负责，
 *      这里先用一个测试侧的小检查器把语义钉死，免得 2.x 写歪了没人发现。
 *   B. 行为语义（0 字节分配、真睡、相对超时、栈太小拒绝、任务能跑能停）。
 *
 * 注意：本文件**故意不 include framework/ 下的任何东西**。契约只靠 platform/port/pf_ops.h
 * 就能验——如果哪天验不动了，说明契约头漏写了东西。
 */
#include "tinytest.h"

#include "platform/host-fake/pf_host_fake.h"

/* ── A. 契约检查器（测试侧的模型）───────────────────────────────── */
enum {
    PF_OK = 0,
    PF_BAD_VERSION = 1,      /* 表布局版本不匹配 */
    PF_BAD_CAPS = 2,         /* 保留位非 0（或 caps 为 NULL） */
    PF_MISSING_MEMBER = 3,   /* 九项里有 NULL */
};

#define PF_REQUIRE(field)                                                     \
    do {                                                                      \
        if (o->field == NULL) {                                               \
            return PF_MISSING_MEMBER;                                         \
        }                                                                     \
    } while (0)

static int pf_contract_check(const pf_ops_t *o)
{
    if (o == NULL) {
        return PF_MISSING_MEMBER;
    }
    if (o->version != PF_OPS_VERSION) {
        return PF_BAD_VERSION;
    }

    PF_REQUIRE(caps);
    PF_REQUIRE(mem_alloc);
    PF_REQUIRE(mem_free);
    PF_REQUIRE(clock_ms);
    PF_REQUIRE(sleep_ms);
    PF_REQUIRE(mutex_create);
    PF_REQUIRE(mutex_take);
    PF_REQUIRE(mutex_give);
    PF_REQUIRE(task_create);
    PF_REQUIRE(task_exit_current);

    /* 只在成员都非 NULL 之后才敢调用它。 */
    if ((o->caps() & PF_CAP_RESERVED_MASK) != 0u) {
        return PF_BAD_CAPS;
    }
    return PF_OK;
}

/* 故意违约的表，用来证明检查器不是"永远返回 0"。 */
static uint32_t caps_with_reserved_bit(void)
{
    return (1u << 10); /* 位 10 是保留位，必须读 0 */
}

/* ── 共享夹具 ───────────────────────────────────────────────────── */
static pf_mutex_t *g_mx;
/* 这几个是**测试替身**：任务线程写、主线程读。用 `volatile` 是错的（不给原子性、
 * 不给 happens-before），一律走 relaxed 原子——测试只要"读到完整的旧值或新值"。 */
static int g_take_result = 123; /* 123 = 还没跑 */
static int g_task_ran;
static int g_exit_before;
static int g_exit_after;

static int g_get(const int *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static void g_set(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

/* 进任务后先睡 10ms，确保持锁者已经把锁拿稳，再试 5ms。 */
static void contender(void *arg)
{
    (void)arg;
    const pf_ops_t *o = pf_host_fake_ops_get();
    o->sleep_ms(10);
    g_set(&g_take_result, o->mutex_take(g_mx, 5));
}

static void simple_task(void *arg)
{
    (void)arg;
    g_set(&g_task_ran, 1);
}

static void exiting_task(void *arg)
{
    (void)arg;
    const pf_ops_t *o = pf_host_fake_ops_get();
    g_set(&g_exit_before, 1);
    o->task_exit_current();
    g_set(&g_exit_after, 1); /* 契约：这行不应被执行 */
}

/* ── A 段用例 ───────────────────────────────────────────────────── */
TT_CASE(host_fake_table_is_conforming)
{
    TT_ASSERT_INT(PF_OK, pf_contract_check(&pf_host_fake_ops));
}

TT_CASE(checker_catches_wrong_version)
{
    pf_ops_t bad = pf_host_fake_ops;
    bad.version = PF_OPS_VERSION + 1u;
    TT_ASSERT_INT(PF_BAD_VERSION, pf_contract_check(&bad));
}

TT_CASE(checker_catches_reserved_caps)
{
    pf_ops_t bad = pf_host_fake_ops;
    bad.caps = caps_with_reserved_bit;
    TT_ASSERT_INT(PF_BAD_CAPS, pf_contract_check(&bad));
}

TT_CASE(checker_catches_missing_member)
{
    pf_ops_t bad = pf_host_fake_ops;
    bad.mutex_give = NULL;
    TT_ASSERT_INT(PF_MISSING_MEMBER, pf_contract_check(&bad));
}

/* ── B 段用例 ───────────────────────────────────────────────────── */
TT_CASE(alloc_zero_returns_null)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    TT_ASSERT_TRUE(o->mem_alloc(0) == NULL);
}

TT_CASE(alloc_then_free_roundtrip)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    unsigned char *p = (unsigned char *)o->mem_alloc(64);
    TT_ASSERT_NOT_NULL(p);

    for (int i = 0; i < 64; i++) {
        p[i] = (unsigned char)(i * 3 + 1);
    }
    for (int i = 0; i < 64; i++) {
        if (p[i] != (unsigned char)(i * 3 + 1)) {
            TT_ASSERT_INT(i, -1); /* 内容被踩了 */
        }
    }
    o->mem_free(p);
    o->mem_free(NULL); /* 契约：NULL 必须无操作 */
}

TT_CASE(clock_ms_is_monotonic)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    uint32_t prev = o->clock_ms();
    for (int i = 0; i < 20; i++) {
        o->sleep_ms(1);
        uint32_t now = o->clock_ms();
        /* 只允许不动或前进；回绕也是"前进"，所以用差值判断。 */
        TT_ASSERT_TRUE((uint32_t)(now - prev) < 1000u);
        prev = now;
    }
}

TT_CASE(sleep_ms_actually_sleeps)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    uint32_t t0 = o->clock_ms();
    o->sleep_ms(25);
    uint32_t elapsed = (uint32_t)(o->clock_ms() - t0);
    /* 契约：允许睡久，不许睡短（±10% 或 ±1 tick，取大者）。这里给足余量，
     * 宿主机负载高时宁可慢，也不能因为抖一下就把门禁弄红。 */
    TT_ASSERT_TRUE(elapsed >= 20u);
    TT_ASSERT_TRUE(elapsed < 500u);
}

TT_CASE(sleep_zero_yields_fast)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    uint32_t t0 = o->clock_ms();
    o->sleep_ms(0);
    TT_ASSERT_TRUE((uint32_t)(o->clock_ms() - t0) < 50u);
}

TT_CASE(mutex_free_lock_acquires_immediately)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    g_mx = o->mutex_create();
    TT_ASSERT_NOT_NULL(g_mx);
    TT_ASSERT_INT(0, o->mutex_take(g_mx, 0));
    o->mutex_give(g_mx);
}

TT_CASE(mutex_take_zero_fails_when_held)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    TT_ASSERT_INT(0, o->mutex_take(g_mx, 0));

    uint32_t t0 = o->clock_ms();
    TT_ASSERT_INT(-1, o->mutex_take(g_mx, 0)); /* 同一任务重复取 = 拿不到 */
    TT_ASSERT_TRUE((uint32_t)(o->clock_ms() - t0) < 50u);

    o->mutex_give(g_mx);
}

TT_CASE(mutex_timeout_is_relative)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    TT_ASSERT_INT(0, o->mutex_take(g_mx, 0));

    g_set(&g_take_result, 123);
    pf_task_desc_t desc = { "contender", contender, NULL, 32768u };
    TT_ASSERT_NOT_NULL(o->task_create(&desc));

    for (int i = 0; i < 40 && g_get(&g_take_result) == 123; i++) {
        o->sleep_ms(5);
    }
    o->mutex_give(g_mx);

    TT_ASSERT_TRUE(g_get(&g_take_result) != 123); /* 跑完了 */
    TT_ASSERT_INT(-1, g_get(&g_take_result)); /* 相对超时：等 5ms 没拿到 */
}

TT_CASE(mutex_reacquire_after_give)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    TT_ASSERT_INT(0, o->mutex_take(g_mx, 5));
    o->mutex_give(g_mx);
}

TT_CASE(task_rejects_small_stack)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    pf_task_desc_t desc = { "small", simple_task, NULL, PF_TASK_MIN_STACK - 1u };
    TT_ASSERT_TRUE(o->task_create(&desc) == NULL);
}

TT_CASE(task_rejects_null_entry)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    pf_task_desc_t desc = { "no-entry", NULL, NULL, 32768u };
    TT_ASSERT_TRUE(o->task_create(&desc) == NULL);
}

TT_CASE(task_runs_entry)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    g_set(&g_task_ran, 0);
    pf_task_desc_t desc = { "runner", simple_task, NULL, 32768u };
    TT_ASSERT_NOT_NULL(o->task_create(&desc));

    for (int i = 0; i < 100 && g_get(&g_task_ran) == 0; i++) {
        o->sleep_ms(5);
    }
    TT_ASSERT_INT(1, g_get(&g_task_ran));
}

TT_CASE(task_exit_current_does_not_return)
{
    const pf_ops_t *o = pf_host_fake_ops_get();
    g_set(&g_exit_before, 0);
    g_set(&g_exit_after, 0);
    pf_task_desc_t desc = { "exiter", exiting_task, NULL, 32768u };
    TT_ASSERT_NOT_NULL(o->task_create(&desc));

    for (int i = 0; i < 100 && g_get(&g_exit_before) == 0; i++) {
        o->sleep_ms(5);
    }
    TT_ASSERT_INT(1, g_get(&g_exit_before));

    o->sleep_ms(30); /* 给"如果它返回了"留出执行完的时间 */
    TT_ASSERT_INT(0, g_get(&g_exit_after));
}

int main(void)
{
    TT_PROGRAM("test_pf_conformance");
    TT_RUN(host_fake_table_is_conforming);
    TT_RUN(checker_catches_wrong_version);
    TT_RUN(checker_catches_reserved_caps);
    TT_RUN(checker_catches_missing_member);
    TT_RUN(alloc_zero_returns_null);
    TT_RUN(alloc_then_free_roundtrip);
    TT_RUN(clock_ms_is_monotonic);
    TT_RUN(sleep_ms_actually_sleeps);
    TT_RUN(sleep_zero_yields_fast);
    TT_RUN(mutex_free_lock_acquires_immediately);
    TT_RUN(mutex_take_zero_fails_when_held);
    TT_RUN(mutex_timeout_is_relative);
    TT_RUN(mutex_reacquire_after_give);
    TT_RUN(task_rejects_small_stack);
    TT_RUN(task_rejects_null_entry);
    TT_RUN(task_runs_entry);
    TT_RUN(task_exit_current_does_not_return);
    return tt_summary();
}
