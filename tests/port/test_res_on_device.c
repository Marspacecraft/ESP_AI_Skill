/* tests/port/test_res_on_device.c —— 资源与配额的真机验收。
 *
 * host 上 `test_res.c` / `test_quota.c` 已经验过语义；这一份补的是**退出路径**：
 * "取消位一置，退出第 ③ 步把 `RES_LIFE_APP` 的槽位收干净、`RES_LIFE_BOOT` 的
 * 槽位原样留着"——这条只有在真调度下走过 INIT→RUNNING→STOPPING→STOPPED 才看得见。
 *
 * 配额那两条同时钉住一个容易混的归属：**`CM_ERR_QUOTA` 是你用超了自己的额度，
 * `CM_ERR_NOMEM` 是整机没了**。两个码必须分得开，否则现场只能靠猜。
 */
#include "tinytest.h"

#include "framework/include/fw_app.h"
#include "framework/include/fw_embed.h"
#include "framework/src/fw_internal.h"
#include "platform/port/pf_ops.h"

static int flag_get(int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void flag_set(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

/* 场景由主任务先设好，App 的 init() 照场景做事，结果写回全局。 */
static int g_scenario;
static int g_done;
static int g_rc1;
static int g_rc2;
static int g_used;
static int g_denied;
static int g_quota;
static int g_used2;
static size_t g_boot_slot_value;

/* 撤销之后任务还该继续跑——计数器由 App 任务自己推，主任务只读。 */
static int g_ticks;

static void wait_done(void)
{
    for (int i = 0; i < 400 && flag_get(&g_done) == 0; i++) {
        pf_platform_ops()->sleep_ms(5u);
    }
}

static cm_err_t res_init(app_ctx_t *ctx)
{
    size_t used = 0u;
    size_t quota = 0u;
    uint32_t denied = 0u;

    switch (g_scenario) {
    case 1: /* 拿到 → 槽位有值；释放 → 槽位清空 */
        g_rc1 = (int)res_acquire(&ctx->slots[0], RES_KIND_MEM, 64u, RES_LIFE_APP);
        fw_res_usage(ctx, &used, &quota, &denied);
        g_used = (int)used;
        res_release(&ctx->slots[0]);
        fw_res_usage(ctx, &used, &quota, &denied);
        g_rc2 = (int)used;
        break;

    case 2: /* 退出时回收 APP 生命周期的槽位（两个） */
        g_rc1 = (int)res_acquire(&ctx->slots[0], RES_KIND_MEM, 32u, RES_LIFE_APP);
        g_rc2 = (int)res_acquire(&ctx->slots[1], RES_KIND_MEM, 32u, RES_LIFE_APP);
        break;

    case 3: /* BOOT 生命周期的槽位：退出时**不许**收 */
        g_rc1 = (int)res_acquire(&ctx->slots[0], RES_KIND_MEM, 32u, RES_LIFE_BOOT);
        g_boot_slot_value = (size_t)(uintptr_t)ctx->slots[0];
        break;

    case 4: /* 配额：先把自己限到 64 字节，再要 128 */
        (void)fw_res_set_quota(ctx, 64u);
        g_rc1 = (fw_alloc(128u) == NULL) ? 1 : 0;
        g_rc2 = (int)res_acquire(&ctx->slots[0], RES_KIND_MEM, 128u, RES_LIFE_APP);
        fw_res_usage(ctx, &used, &quota, &denied);
        g_used = (int)used;
        g_denied = (int)denied;
        break;

    case 5: /* 撤销：额度压到当前用量；再要更多就是 QUOTA,已用的不回收 */
        {
            void *p = fw_alloc(64u);

            g_rc1 = (p != NULL) ? (int)CM_OK : -1;
            (void)fw_res_revoke(ctx);
            fw_res_usage(ctx, &used, &quota, &denied);
            g_quota = (int)quota;              /* 压到 64,不是 0(0 = 不限) */
            g_rc2 = (fw_alloc(8u) == NULL) ? 1 : 0;
            fw_res_usage(ctx, &used, &quota, &denied);
            g_used = (int)used;                /* 已用 64 不被撤销收回 */
            g_denied = (int)denied;
            fw_free(p);
        }
        break;

    case 6: /* 撤销不杀任务:init 期间状态还是 INIT,所以只撤销,状态由用例读 */
        (void)fw_res_revoke(ctx);
        g_rc1 = (int)CM_OK;
        break;

    case 7: /* 零用量时撤销:压到 1 字节,而不是 0(0 会变成"不限") */
        (void)fw_res_revoke(ctx);
        fw_res_usage(ctx, &used, &quota, &denied);
        g_quota = (int)quota;
        g_rc1 = (fw_alloc(2u) == NULL) ? 1 : 0;
        {
            void *p = fw_alloc(1u);

            g_rc2 = (p != NULL) ? 1 : 0;
            fw_free(p);
        }
        break;

    case 8: /* 撤销是"上限",不是"拉黑":释放之后同样大小还能再要 */
        {
            void *p = fw_alloc(64u);

            (void)fw_res_revoke(ctx);
            fw_free(p);
            {
                void *q = fw_alloc(64u);

                g_rc1 = (q != NULL) ? 1 : 0;
                fw_free(q);
            }
            fw_res_usage(ctx, &used, &quota, &denied);
            g_used = (int)used;
            g_used2 = (int)quota;
        }
        break;

    default:
        break;
    }
    flag_set(&g_done, 1);
    return CM_OK;
}

static void res_exit(app_ctx_t *ctx) { (void)ctx; }
static void res_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
        fw_delay_block(50u);
    }
}

static const app_desc_t res_desc = {
    .id = 47u, .name = "res", .init = res_init, .exit = res_exit, .task = res_task,
    .stack_bytes = APP_STACK_MIN, .flags = APP_FLAG_SYSTEM,
};

static struct fw_app_ctx *start(int scenario)
{
    static const app_desc_t *apps[1];
    fw_manifest_t m;

    fw_reset();
    g_scenario = scenario;
    g_done = 0;
    g_rc1 = -1;
    g_rc2 = -1;
    g_used = -1;
    g_denied = -1;
    g_quota = -1;
    g_used2 = -1;
    g_ticks = 0;
    g_boot_slot_value = 0u;

    apps[0] = &res_desc;
    m.apps = apps;
    m.count = 1u;
    if (fw_embed_start(&m) != CM_OK) {
        return NULL;
    }
    wait_done();
    return fw_live_at(0u);
}

/* 取消之后必须等到**终态**（STOPPED/FAILED），不能停在 STOPPING：
 * 退出五步里 ①②③ 还在跑，此时读 `slots[]` 与 `fw_app_slots_released()` 会读到
 * "回收了一半"的中间态。这条在 host 上偶发过：`期望 4（STOPPED），实际 3（STOPPING）`。 */
static void stop_it(struct fw_app_ctx *c)
{
    fw_app_state_e st;

    if (c == NULL) {
        return;
    }
    fw_app_request_cancel(c);
    for (int i = 0; i < 400; i++) {
        st = fw_app_state(c);
        if (st == FW_APP_STATE_STOPPED || st == FW_APP_STATE_FAILED) {
            return;
        }
        pf_platform_ops()->sleep_ms(5u);
    }
}

static void reap(struct fw_app_ctx *c)
{
    if (c != NULL) {
        fw_live_remove(c);
        fw_app_ctx_free(c);
    }
    fw_reset();
}

/* ------------------------------------------------------------------ */

TT_CASE(acquire_then_release_frees_the_slot_and_the_quota_usage)
{
    struct fw_app_ctx *c = start(1);

    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT(1, flag_get(&g_done));
    TT_ASSERT_INT((int)CM_OK, g_rc1);
    TT_ASSERT_INT(64, g_used);      /* 拿到那一刻，用量确实是 64 */
    TT_ASSERT_INT(0, g_rc2);        /* 释放之后回到 0 */
    stop_it(c);
    reap(c);
}

TT_CASE(exit_releases_app_lifetime_slots_exactly_once)
{
    struct fw_app_ctx *c = start(2);
    uint32_t released;

    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT((int)CM_OK, g_rc1);
    TT_ASSERT_INT((int)CM_OK, g_rc2);
    TT_ASSERT_TRUE(c->pub.slots[0] != NULL);

    stop_it(c);
    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, (int)fw_app_state(c));

    /* 退出第 ③ 步的观测口：两个槽位都被收，且计数恰好 2（不是 4、不是 0）。 */
    released = fw_app_slots_released(c);
    TT_ASSERT_INT(2, (int)released);
    TT_ASSERT_TRUE(c->pub.slots[0] == NULL);
    TT_ASSERT_TRUE(c->pub.slots[1] == NULL);
    reap(c);
}

TT_CASE(boot_lifetime_slot_survives_exit)
{
    struct fw_app_ctx *c = start(3);
    uint32_t released;

    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT((int)CM_OK, g_rc1);
    TT_ASSERT_TRUE(c->pub.slots[0] != NULL);

    stop_it(c);
    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, (int)fw_app_state(c));

    /* 借用整机共享外设：框架**不许**替调用方归还，槽位必须还在。 */
    released = fw_app_slots_released(c);
    TT_ASSERT_INT(0, (int)released);
    TT_ASSERT_TRUE(c->pub.slots[0] != NULL);

    fw_free(c->pub.slots[0]);   /* 借用者自己交还 */
    reap(c);
}

TT_CASE(quota_denial_is_quota_not_nomem)
{
    struct fw_app_ctx *c = start(4);

    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT(1, g_rc1);                        /* `fw_alloc(128)` 在 64 的额度下给 NULL */
    TT_ASSERT_INT((int)CM_ERR_QUOTA, g_rc2);        /* 而 `res_acquire` 能说清是哪种失败 */
    TT_ASSERT_INT(0, g_used);                       /* 被拒的分配不占额度 */
    /* 两次尝试各记一次拒绝：`fw_alloc(128)` 一次、`res_acquire(128)` 一次。 */
    TT_ASSERT_INT(2, g_denied);
    stop_it(c);
    reap(c);
}

/* ---------------------------------------------------------------- 撤销 */

TT_CASE(revoke_caps_allocations_at_current_usage)
{
    struct fw_app_ctx *c = start(5);

    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT((int)CM_OK, g_rc1);
    TT_ASSERT_INT(64, g_quota);      /* 撤销 = 把上限压到"此刻已用" */
    TT_ASSERT_INT(1, g_rc2);         /* 再要 8 字节:拒 */
    TT_ASSERT_INT(64, g_used);       /* 已占的那 64 字节不被撤销收回 */
    TT_ASSERT_INT(1, g_denied);      /* 被拒的分配记一次 */
    stop_it(c);
    reap(c);
}

TT_CASE(revoke_does_not_stop_the_app)
{
    struct fw_app_ctx *c;

    c = start(6);
    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT((int)CM_OK, g_rc1);
    TT_ASSERT_INT((int)FW_APP_STATE_RUNNING, (int)fw_app_state(c));  /* 撤销不改状态 */
    TT_ASSERT_INT(0, (int)__atomic_load_n(&c->cancel, __ATOMIC_ACQUIRE));
    /* 撤销之后任务继续跑:计数必须还在涨。
     *
     * 这里**不能**用固定墙钟等待（"睡 120ms 再看"）：App 每轮 `fw_delay_block(50u)`，
     * 慢 runner 上 120ms 未必排得进两次切片，断言就会随机红——第一版就是这么红的
     * （macOS CI：`revoke_does_not_stop_the_app -- 期望为真，实际为假`）。
     * 改成"最多等 2s 的有界轮询"：要证明的是"它还在涨"，不是"它在 120ms 内涨了两次"。
     */
    {
        int before = flag_get(&g_ticks);
        int grew = 0;

        for (int i = 0; i < 400; i++) {
            pf_platform_ops()->sleep_ms(5u);
            if (flag_get(&g_ticks) > before) {
                grew = 1;
                break;
            }
        }
        TT_ASSERT_TRUE(grew);
    }
    TT_ASSERT_INT((int)FW_APP_STATE_RUNNING, (int)fw_app_state(c));
    stop_it(c);
    reap(c);
}

TT_CASE(revoke_on_zero_usage_is_one_byte_not_unlimited)
{
    struct fw_app_ctx *c = start(7);

    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT(1, g_quota);       /* 压到 0 会变成"不限",所以取 1 */
    TT_ASSERT_INT(1, g_rc1);         /* 2 字节:拒 */
    TT_ASSERT_INT(1, g_rc2);         /* 1 字节:还允许 */
    stop_it(c);
    reap(c);
}

TT_CASE(revoke_is_a_ceiling_not_a_blacklist)
{
    struct fw_app_ctx *c = start(8);

    TT_ASSERT_TRUE(c != NULL);
    TT_ASSERT_INT(1, g_rc1);         /* 释放之后,同样大小还能再要 */
    TT_ASSERT_INT(0, g_used);        /* 用掉的那次已归还 */
    TT_ASSERT_INT(64, g_used2);      /* 上限仍是 64,没被释放动作改掉 */
    stop_it(c);
    reap(c);
}

#if defined(ESP_PLATFORM)
int res_on_device_main(void)
#else
int main(void)
#endif
{
    TT_PROGRAM("test_res_on_device");
    TT_RUN(acquire_then_release_frees_the_slot_and_the_quota_usage);
    TT_RUN(exit_releases_app_lifetime_slots_exactly_once);
    TT_RUN(boot_lifetime_slot_survives_exit);
    TT_RUN(quota_denial_is_quota_not_nomem);
    TT_RUN(revoke_caps_allocations_at_current_usage);
    TT_RUN(revoke_does_not_stop_the_app);
    TT_RUN(revoke_on_zero_usage_is_one_byte_not_unlimited);
    TT_RUN(revoke_is_a_ceiling_not_a_blacklist);
    return tt_summary();
}
