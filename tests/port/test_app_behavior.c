/* tests/port/test_app_behavior.c —— 框架行为的真机验收（步骤 9.4）。
 *
 * 端口套件（`test_port_conformance.c`）验的是"这张表说得对不对"；这一份验的是
 * **建立在这张表上的框架**在真硬件上是否真的做到了它承诺的事，尤其是那条
 * 只有在真实调度下才出现的承诺：
 *
 *     取消位一置，`fw_delay_block()` 在有限时间内结束任务，`exit()` 恰好一次，
 *     之后这个 App 一行代码都不再执行。
 *
 * host 上这条已经绿了；但 host 是 pthread + 我们自己写的 `sleep_ms`，真机是
 * FreeRTOS 的 tick、抢占与 20 ms 切片。**"host 通过"不能推"板上通过"**，
 * 所以同一份断言要在两块地方各跑一遍。
 */
#include "tinytest.h"

#include "framework/include/fw_app.h"
#include "framework/include/fw_embed.h"
#include "framework/src/fw_internal.h"
#include "platform/port/pf_ops.h"

/* ------------------------------------------------------------------ */
/* 观测点：跨任务读写，全部走原子（否则 TSan 与真机都可能给出假绿）        */
/* ------------------------------------------------------------------ */

static int g_init_calls;
static int g_exit_calls;
static int g_ticks;

static int obs_get(const int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void obs_add(int *p) { (void)__atomic_add_fetch(p, 1, __ATOMIC_RELAXED); }

/* ------------------------------------------------------------------ */
/* 被测 App                                                           */
/* ------------------------------------------------------------------ */

static cm_err_t ok_init(app_ctx_t *ctx) { obs_add(&g_init_calls); ctx->slots[0] = ctx; return CM_OK; }
static void ok_exit(app_ctx_t *ctx) { (void)ctx; obs_add(&g_exit_calls); }
static void ok_task(app_ctx_t *ctx) { (void)ctx; for (;;) { obs_add(&g_ticks); fw_delay_block(20u); } }

static cm_err_t bad_init(app_ctx_t *ctx) { (void)ctx; obs_add(&g_init_calls); return CM_ERR_INVALID; }

/* 一次要 1000 ms 的延时：用来证明"切片"真的把长等待切碎了。 */
static void slow_task(app_ctx_t *ctx) { (void)ctx; obs_add(&g_ticks); fw_delay_block(1000u); obs_add(&g_ticks); }

static const app_desc_t ok_desc = {
    .id = 40u, .name = "beh", .init = ok_init, .exit = ok_exit, .task = ok_task,
    .stack_bytes = APP_STACK_MIN, .flags = APP_FLAG_SYSTEM,
};
static const app_desc_t bad_desc = {
    .id = 41u, .name = "beh_bad", .init = bad_init, .exit = ok_exit, .task = ok_task,
    .stack_bytes = APP_STACK_MIN, .flags = APP_FLAG_SYSTEM,
};
static const app_desc_t slow_desc = {
    .id = 42u, .name = "beh_slow", .init = ok_init, .exit = ok_exit, .task = slow_task,
    .stack_bytes = APP_STACK_MIN, .flags = APP_FLAG_SYSTEM,
};

static fw_manifest_t manifest_of(const app_desc_t *one)
{
    static const app_desc_t *apps[1];
    fw_manifest_t m;

    apps[0] = one;
    m.apps = apps;
    m.count = 1u;
    return m;
}

/* ------------------------------------------------------------------ */
/* 夹具                                                               */
/* ------------------------------------------------------------------ */

static void reset_all(void)
{
    while (fw_live_count() > 0u) {
        struct fw_app_ctx *c = fw_live_at(0u);

        fw_app_request_cancel(c);
        for (int i = 0; i < 400; i++) {
            fw_app_state_e st = fw_app_state(c);

            if (st == FW_APP_STATE_STOPPED || st == FW_APP_STATE_FAILED) {
                break;
            }
            pf_platform_ops()->sleep_ms(5u);
        }
        fw_live_remove(c);
        fw_app_ctx_free(c);
    }
    fw_reset();
    /* 计数清零放在**结束后**：停实例本身会让 `exit()` 加一，
     * 一开始清零会把这次的 exit 也算进下一个用例。 */
    g_init_calls = 0;
    g_exit_calls = 0;
    g_ticks = 0;
}

static uint32_t now_ms(void) { return pf_platform_ops()->clock_ms(); }

static int wait_until_truthy(const int *p, int want, uint32_t timeout_ms)
{
    uint32_t t0 = now_ms();

    while ((uint32_t)(now_ms() - t0) < timeout_ms) {
        if (obs_get(p) >= want) {
            return 1;
        }
        pf_platform_ops()->sleep_ms(5u);
    }
    return obs_get(p) >= want;
}

static int wait_terminal(struct fw_app_ctx *c, uint32_t timeout_ms)
{
    uint32_t t0 = now_ms();

    while ((uint32_t)(now_ms() - t0) < timeout_ms) {
        fw_app_state_e st = fw_app_state(c);

        if (st == FW_APP_STATE_STOPPED || st == FW_APP_STATE_FAILED) {
            return (int)st;
        }
        pf_platform_ops()->sleep_ms(5u);
    }
    return (int)fw_app_state(c);
}

/* ------------------------------------------------------------------ */
/* 用例                                                               */
/* ------------------------------------------------------------------ */

TT_CASE(app_starts_and_runs_on_the_device)
{
    reset_all();
    fw_manifest_t m = manifest_of(&ok_desc);

    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    TT_ASSERT_TRUE(wait_until_truthy(&g_init_calls, 1, 500u));
    TT_ASSERT_TRUE(wait_until_truthy(&g_ticks, 1, 500u));
    TT_ASSERT_INT(1u, fw_live_count());
    reset_all();
}

TT_CASE(cancel_stops_the_task_and_calls_exit_exactly_once)
{
    reset_all();
    fw_manifest_t m = manifest_of(&ok_desc);
    struct fw_app_ctx *c;

    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    TT_ASSERT_TRUE(wait_until_truthy(&g_ticks, 2, 1000u));

    c = fw_live_at(0u);
    TT_ASSERT_TRUE(c != NULL);
    fw_app_request_cancel(c);

    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, wait_terminal(c, 1000u));
    TT_ASSERT_INT(1, obs_get(&g_init_calls));
    TT_ASSERT_INT(1, obs_get(&g_exit_calls));

    /* 「结束了」不等于「不再执行」：再等 5 个切片，计数必须冻住。 */
    {
        int before = obs_get(&g_ticks);

        pf_platform_ops()->sleep_ms(120u);
        TT_ASSERT_INT(before, obs_get(&g_ticks));
    }
    reset_all();
}

TT_CASE(long_delay_is_cut_into_slices)
{
    reset_all();
    fw_manifest_t m = manifest_of(&slow_desc);
    struct fw_app_ctx *c;
    uint32_t t0;
    int st;

    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    TT_ASSERT_TRUE(wait_until_truthy(&g_ticks, 1, 1000u));

    c = fw_live_at(0u);
    TT_ASSERT_TRUE(c != NULL);
    pf_platform_ops()->sleep_ms(60u);

    t0 = now_ms();
    fw_app_request_cancel(c);
    st = wait_terminal(c, 2000u);

    /* 1000 ms 的等待必须在一小段切片内被放弃；留足 400 ms 抗抖动。 */
    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, st);
    TT_ASSERT_TRUE((uint32_t)(now_ms() - t0) < 400u);
    /* 任务被取消后不该再跑到 `fw_delay_block()` 后面那一行。 */
    TT_ASSERT_INT(1, obs_get(&g_ticks));
    TT_ASSERT_INT(1, obs_get(&g_exit_calls));
    reset_all();
}

TT_CASE(init_failure_becomes_failed_and_exits_once)
{
    reset_all();
    fw_manifest_t m = manifest_of(&bad_desc);
    struct fw_app_ctx *c;

    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    c = fw_live_at(0u);
    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT((int)FW_APP_STATE_FAILED, wait_terminal(c, 1000u));
    TT_ASSERT_INT(1, obs_get(&g_exit_calls));
    TT_ASSERT_INT(0, obs_get(&g_ticks));
    reset_all();
}

/* 固件里只允许有一个 `main`（本工程是 ESP-IDF 的 `app_main` 入口，`main` 这个
 * 符号留给套件）。两个套件都要各自的入口名，所以按"是不是在 IDF 里"选择：
 * 设备侧叫 `app_behavior_main`，host 侧仍是 `main`。 */
#if defined(ESP_PLATFORM)
int app_behavior_main(void)
#else
int main(void)
#endif
{
    TT_PROGRAM("test_app_behavior");
    TT_RUN(app_starts_and_runs_on_the_device);
    TT_RUN(cancel_stops_the_task_and_calls_exit_exactly_once);
    TT_RUN(long_delay_is_cut_into_slices);
    TT_RUN(init_failure_becomes_failed_and_exits_once);
    return tt_summary();
}
