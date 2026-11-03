/* tests/framework/test_fw_delay.c —— 步骤 2.4 的验收：伪阻塞、取消、退出路径。
 *
 * 这一份测的是框架里最难用眼睛看出对错的一段：App 看起来在"睡"，其实是被
 * 切成片在等；取消发生在某一片**里面**；而取消之后，`fw_delay_block()` 后面
 * 那行代码永远不执行。所以每条用例都在验证"不该发生的事没发生"：
 *   - 取消之后 `exit()` 恰好一次（不是零次，也不是两次）；
 *   - `exit()` 之后那行没跑；
 *   - 取消位被清掉了（否则 `exit()` 里的阻塞会再命中一次）；
 *   - 每片都不超过 `FW_DELAY_SLICE_MS`。
 *
 * 时间由 `clock_fake` 管：`sleep_ms` 只推虚拟时间，用例不会真的等。取消在
 * 第 N 次 `sleep_ms` 之后注入（`clock_fake_set_sleep_hook`），这样注入点落在
 * **切片中间**，而不是调用之前——后者测不出"最后一片里被取消"这个坑。
 *
 * 等待任务结束用真表的 `clock_ms()` 轮询（2s 超时）：`tinytest` 没有超时保护，
 * 这里自己给一条，否则真挂住时 CI 只会挂到天荒地老。
 */
#include "tinytest.h"
#include "tests/support/clock_fake.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

/* ── 用例共享状态 ─────────────────────────────────────────────── */
static struct fw_app_ctx *g_ctx;
static uint32_t g_cancel_at;          /* 第几次 sleep 之后注入取消；0 = 不注入 */
/* 任务线程写、主线程读：`volatile` 不构成同步，一律 relaxed 原子。 */
static int g_init_calls;
static int g_exit_calls;
static int g_after_delay;    /* 不该被执行的标记 */
static int g_exit_blocked;   /* exit() 里再调阻塞后是否返回了 */
static int g_done;           /* 正常跑完标记 */

static int g_get(const int *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static void g_set(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

/* ── 回调 ─────────────────────────────────────────────────────── */
static cm_err_t cb_init_ok(app_ctx_t *ctx)
{
    (void)ctx;
    (void)__atomic_add_fetch(&g_init_calls, 1, __ATOMIC_RELAXED);
    return CM_OK;
}

static cm_err_t cb_init_fail(app_ctx_t *ctx)
{
    (void)ctx;
    return CM_ERR_INVALID;
}

/* exit() 里故意再调一次阻塞接口：验证"退出期间取消位失效"且不死锁。 */
static void cb_exit(app_ctx_t *ctx)
{
    (void)ctx;
    (void)__atomic_add_fetch(&g_exit_calls, 1, __ATOMIC_RELAXED);
    fw_delay_block(0u);
    g_set(&g_exit_blocked, 1);
}

/* 永不返回的主任务：每次 1000ms（= 50 片），被取消时应当停在片中间。 */
static void cb_task_forever(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        fw_delay_block(1000u);
        (void)__atomic_add_fetch(&g_after_delay, 1, __ATOMIC_RELAXED);
    }
}

static void cb_task_return(app_ctx_t *ctx)
{
    (void)ctx;
}

static void cb_task_short(app_ctx_t *ctx)
{
    (void)ctx;
    fw_delay_block(100u);
    g_set(&g_done, 1);
}

/* ── 夹具 ─────────────────────────────────────────────────────── */
static void cancel_hook(void)
{
    if (g_cancel_at != 0u && clock_fake_sleep_calls() == g_cancel_at) {
        fw_app_request_cancel(g_ctx);
    }
}

static int setup(const app_desc_t *d)
{
    fw_reset();
    clock_fake_install(pf_host_fake_ops_get());
    clock_fake_set_sleep_hook(NULL);
    g_cancel_at = 0u;
    g_set(&g_init_calls, 0);
    g_set(&g_exit_calls, 0);
    g_set(&g_after_delay, 0);
    g_set(&g_exit_blocked, 0);
    g_set(&g_done, 0);
    if (fw_platform_bind(clock_fake_ops()) != CM_OK) {
        return 0;
    }
    g_ctx = fw_app_ctx_new(d);
    return g_ctx != NULL;
}

static void teardown(void)
{
    clock_fake_set_sleep_hook(NULL);
    if (g_ctx != NULL) {
        fw_app_ctx_free(g_ctx);
        g_ctx = NULL;
    }
    fw_reset();
}

/* 用**真表**的时间等状态变成 want；超时返回 0。 */
static int wait_state(fw_app_state_e want, uint32_t timeout_ms)
{
    const pf_ops_t *real = pf_host_fake_ops_get();
    uint32_t t0 = real->clock_ms();

    while (fw_app_state(g_ctx) != want) {
        if ((uint32_t)(real->clock_ms() - t0) > timeout_ms) {
            return 0;
        }
        real->sleep_ms(1u);
    }
    return 1;
}

static int wait_flag(const int *flag, uint32_t timeout_ms)
{
    const pf_ops_t *real = pf_host_fake_ops_get();
    uint32_t t0 = real->clock_ms();

    while (g_get(flag) == 0) {
        if ((uint32_t)(real->clock_ms() - t0) > timeout_ms) {
            return 0;
        }
        real->sleep_ms(1u);
    }
    return 1;
}

/* ── 描述体 ───────────────────────────────────────────────────── */
static const app_desc_t k_forever = {
    7u, "forever", cb_init_ok, cb_exit, cb_task_forever, 0u, APP_STACK_MIN};
static const app_desc_t k_init_fail = {
    8u, "init_fail", cb_init_fail, cb_exit, cb_task_forever, 0u, APP_STACK_MIN};
static const app_desc_t k_return = {
    9u, "ret", cb_init_ok, cb_exit, cb_task_return, 0u, APP_STACK_MIN};
static const app_desc_t k_short = {
    10u, "short", cb_init_ok, cb_exit, cb_task_short, 0u, APP_STACK_MIN};

/* ── 用例 ─────────────────────────────────────────────────────── */

/* 取消落在第 3 片之后：exit 一次、后面的行没跑、终态 STOPPED、取消位已清。 */
TT_CASE(cancel_stops_at_slice_boundary)
{
    TT_ASSERT_TRUE(setup(&k_forever));
    g_cancel_at = 3u;
    clock_fake_set_sleep_hook(cancel_hook);

    TT_ASSERT_INT(CM_OK, fw_app_start(g_ctx));
    if (!wait_state(FW_APP_STATE_STOPPED, 2000u)) {
        TT_ASSERT_INT(FW_APP_STATE_STOPPED, fw_app_state(g_ctx));
    }

    TT_ASSERT_INT(1, g_get(&g_init_calls));
    TT_ASSERT_INT(1, g_get(&g_exit_calls));
    TT_ASSERT_INT(1u, fw_app_exit_calls(g_ctx));
    TT_ASSERT_INT(0, g_get(&g_after_delay));
    TT_ASSERT_INT(1, g_get(&g_exit_blocked));
    TT_ASSERT_INT(0u, g_ctx->cancel);          /* ① 先清位 */
    TT_ASSERT_TRUE(clock_fake_max_slice_ms() <= FW_DELAY_SLICE_MS);
    teardown();
}

/* 取消两次，`exit()` 仍然只调用一次。 */
TT_CASE(double_cancel_still_one_exit)
{
    TT_ASSERT_TRUE(setup(&k_forever));
    fw_app_request_cancel(g_ctx);
    fw_app_request_cancel(g_ctx);

    TT_ASSERT_INT(CM_OK, fw_app_start(g_ctx));
    if (!wait_state(FW_APP_STATE_STOPPED, 2000u)) {
        TT_ASSERT_INT(FW_APP_STATE_STOPPED, fw_app_state(g_ctx));
    }

    TT_ASSERT_INT(1, g_get(&g_exit_calls));
    TT_ASSERT_INT(1u, fw_app_exit_calls(g_ctx));
    TT_ASSERT_INT(0, g_get(&g_after_delay));
    teardown();
}

/* init 失败也是"结束路径"：终态 FAILED，exit 一次。 */
TT_CASE(init_failure_goes_failed_and_exits_once)
{
    TT_ASSERT_TRUE(setup(&k_init_fail));

    TT_ASSERT_INT(CM_OK, fw_app_start(g_ctx));
    if (!wait_state(FW_APP_STATE_FAILED, 2000u)) {
        TT_ASSERT_INT(FW_APP_STATE_FAILED, fw_app_state(g_ctx));
    }

    TT_ASSERT_INT(1, g_get(&g_exit_calls));
    TT_ASSERT_INT(1, g_get(&g_exit_blocked));          /* exit() 里的阻塞返回了 */
    teardown();
}

/* task() 返回 = 自认失败：终态 FAILED，exit 一次。 */
TT_CASE(task_return_goes_failed_and_exits_once)
{
    TT_ASSERT_TRUE(setup(&k_return));

    TT_ASSERT_INT(CM_OK, fw_app_start(g_ctx));
    if (!wait_state(FW_APP_STATE_FAILED, 2000u)) {
        TT_ASSERT_INT(FW_APP_STATE_FAILED, fw_app_state(g_ctx));
    }

    TT_ASSERT_INT(1, g_get(&g_exit_calls));
    teardown();
}

/* 正常跑完：100ms 被切成 20+20+20+20+20，不多不少。 */
TT_CASE(short_delay_completes_without_cancel)
{
    TT_ASSERT_TRUE(setup(&k_short));

    TT_ASSERT_INT(CM_OK, fw_app_start(g_ctx));
    if (!wait_flag(&g_done, 2000u)) {
        TT_ASSERT_INT(1, g_get(&g_done));
    }
    if (!wait_state(FW_APP_STATE_FAILED, 2000u)) {
        TT_ASSERT_INT(FW_APP_STATE_FAILED, fw_app_state(g_ctx));
    }

    TT_ASSERT_INT(100u, clock_fake_sleep_total_ms());
    TT_ASSERT_INT(5u, clock_fake_slice_count());
    TT_ASSERT_TRUE(clock_fake_max_slice_ms() <= FW_DELAY_SLICE_MS);
    teardown();
}

/* 主线程（没有 ctx）：照样切片，最长一片不超过 20ms。 */
TT_CASE(no_context_still_slices)
{
    fw_reset();
    clock_fake_install(pf_host_fake_ops_get());
    clock_fake_set_sleep_hook(NULL);
    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));
    TT_ASSERT_TRUE(fw_current_ctx() == NULL);

    fw_delay_block(50u);
    TT_ASSERT_INT(50u, clock_fake_sleep_total_ms());
    TT_ASSERT_TRUE(clock_fake_max_slice_ms() <= FW_DELAY_SLICE_MS);

    fw_reset();
}

/* ms==0：只让出一次，不推进时间、不记切片。 */
TT_CASE(zero_delay_yields_once)
{
    fw_reset();
    clock_fake_install(pf_host_fake_ops_get());
    clock_fake_set_sleep_hook(NULL);
    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));

    fw_delay_block(0u);
    TT_ASSERT_INT(1u, clock_fake_sleep_calls());
    TT_ASSERT_INT(0u, clock_fake_sleep_total_ms());
    TT_ASSERT_INT(0u, clock_fake_slice_count());

    fw_reset();
}

int main(void)
{
    TT_PROGRAM("test_fw_delay");

    TT_RUN(cancel_stops_at_slice_boundary);
    TT_RUN(double_cancel_still_one_exit);
    TT_RUN(init_failure_goes_failed_and_exits_once);
    TT_RUN(task_return_goes_failed_and_exits_once);
    TT_RUN(short_delay_completes_without_cancel);
    TT_RUN(no_context_still_slices);
    TT_RUN(zero_delay_yields_once);
    return tt_summary();
}
