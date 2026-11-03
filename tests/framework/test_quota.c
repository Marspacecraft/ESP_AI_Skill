/* tests/framework/test_quota.c —— 配额与撤销（步骤 6.2）。
 *
 * 要证明的三件事：
 *   ① 超配额是 `CM_ERR_QUOTA`，**不是** `CM_ERR_NOMEM`（两者现场含义相反）；
 *   ② 记账是准的（分配 +、释放 −），所以"还了就能再借"；
 *   ③ `fw_res_revoke()` 之后任何新分配都失败，但已经在跑的任务不被杀。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

static struct fw_app_ctx *g_ctx;
static int g_scenario;
static int g_done;
static int g_ok1, g_ok2, g_ok3;
static size_t g_used_after_alloc, g_used_after_free;

static void quota_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        fw_delay_block(20u);
    }
}

/* 每种场景在 init 里跑一遍：init 跑在 App 自己的任务里，`fw_current_ctx()`
 * 才有值（`fw_alloc` 要靠它记账）。 */
static cm_err_t quota_init(app_ctx_t *ctx)
{
    size_t used = 0u;
    uint32_t denied = 0u;

    switch (g_scenario) {
    case 0: { /* 不限配额：能分配、记账、释放后归零 */
        void *p = fw_alloc(64u);

        g_ok1 = (p != NULL) ? 1 : 0;
        fw_res_usage(ctx, &g_used_after_alloc, NULL, NULL);
        fw_free(p);
        fw_res_usage(ctx, &g_used_after_free, NULL, NULL);
        break;
    }
    case 1: { /* 配额 100：64 能拿，再 64 被拒，拒绝计数 +1 */
        void *a = fw_alloc(64u);
        void *b = fw_alloc(64u);

        g_ok1 = (a != NULL) ? 1 : 0;
        g_ok2 = (b == NULL) ? 1 : 0;
        fw_res_usage(ctx, &used, NULL, &denied);
        g_ok3 = (denied == 1u) ? 1 : 0;
        g_used_after_alloc = used;
        fw_free(a);
        fw_res_usage(ctx, &g_used_after_free, NULL, NULL);
        break;
    }
    case 2: { /* 配额 64：拿满 → 还掉 → 还能再拿满（额度是真的还回来了） */
        void *a = fw_alloc(64u);

        g_ok1 = (a != NULL) ? 1 : 0;
        fw_free(a);
        g_ok2 = (fw_alloc(64u) != NULL) ? 1 : 0;
        break;
    }
    case 3: { /* 资源获取走同一套记账：超配额必须是 QUOTA，不是 NOMEM */
        cm_err_t rc = res_acquire(&ctx->slots[0], RES_KIND_MEM, 64u, RES_LIFE_APP);

        g_ok1 = (rc == CM_ERR_QUOTA) ? 1 : 0;
        g_ok2 = (ctx->slots[0] == NULL) ? 1 : 0; /* 失败不许留登记 */
        break;
    }
    case 4: { /* 撤销之后：任何新分配都失败；任务照样在跑 */
        g_ok1 = (fw_alloc(8u) == NULL) ? 1 : 0;
        break;
    }
    default:
        break;
    }
    __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
    return CM_OK;
}

static void quota_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static const app_desc_t quota_desc = {
    40u, "quota", quota_init, quota_exit, quota_task, 0u, APP_STACK_MIN,
};

static void stop_all(void)
{
    while (fw_live_count() > 0u) {
        struct fw_app_ctx *c = fw_live_at(0u);

        fw_app_request_cancel(c);
        for (int i = 0; i < 400; i++) {
            if (fw_app_state(c) == FW_APP_STATE_STOPPED || fw_app_state(c) == FW_APP_STATE_FAILED) {
                break;
            }
            pf_host_fake_ops_get()->sleep_ms(2u);
        }
        fw_live_remove(c);
        fw_app_ctx_free(c);
    }
    fw_reset();
}

static void setup(int scenario)
{
    fw_reset();
    g_ctx = NULL;
    g_scenario = scenario;
    g_done = 0;
    g_ok1 = g_ok2 = g_ok3 = 0;
    g_used_after_alloc = g_used_after_free = 0u;
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
}

/* 建实例 → 设配额 → 启动 → 等 init 把场景跑完。 */
static void run_scenario(int scenario, size_t quota)
{
    setup(scenario);
    g_ctx = fw_app_ctx_new_with_id(&quota_desc, 40u);
    TT_ASSERT_NOT_NULL(g_ctx);
    if (quota != 0u) {
        TT_ASSERT_INT(CM_OK, fw_res_set_quota(&g_ctx->pub, quota));
    }
    if (scenario == 4) {
        /* 先给一个宽松配额，再撤销：验证"撤销 = 压到当前用量"。 */
        TT_ASSERT_INT(CM_OK, fw_res_set_quota(&g_ctx->pub, 1024u));
        TT_ASSERT_INT(CM_OK, fw_res_revoke(&g_ctx->pub));
    }
    TT_ASSERT_INT(CM_OK, fw_app_start(g_ctx));
    for (int i = 0; i < 400 && !__atomic_load_n(&g_done, __ATOMIC_ACQUIRE); i++) {
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT(1, __atomic_load_n(&g_done, __ATOMIC_ACQUIRE));
}

TT_CASE(unlimited_quota_allocates_and_accounts)
{
    run_scenario(0, 0u);
    TT_ASSERT_INT(1, g_ok1);
    TT_ASSERT_INT(64, (int)g_used_after_alloc);
    TT_ASSERT_INT(0, (int)g_used_after_free); /* 还回去，账必须归零 */
    stop_all();
}

TT_CASE(over_quota_is_quota_error_and_counts_denial)
{
    run_scenario(1, 100u);
    TT_ASSERT_INT(1, g_ok1);
    TT_ASSERT_INT(1, g_ok2); /* 第二次 64 被拒 */
    TT_ASSERT_INT(1, g_ok3); /* 拒绝次数记了 1 */
    TT_ASSERT_INT(64, (int)g_used_after_alloc);
    TT_ASSERT_INT(0, (int)g_used_after_free);
    stop_all();
}

TT_CASE(freed_bytes_come_back)
{
    run_scenario(2, 64u);
    TT_ASSERT_INT(1, g_ok1);
    TT_ASSERT_INT(1, g_ok2); /* 还掉之后同样的额度又借得出来 */
    stop_all();
}

TT_CASE(res_acquire_reports_quota_not_nomem)
{
    run_scenario(3, 32u);
    TT_ASSERT_INT(1, g_ok1); /* CM_ERR_QUOTA */
    TT_ASSERT_INT(1, g_ok2); /* 槽位保持空着 */
    stop_all();
}

TT_CASE(revoke_blocks_new_allocations_but_does_not_kill)
{
    run_scenario(4, 0u);
    TT_ASSERT_INT(1, g_ok1); /* 撤销之后 8 字节也拿不到 */
    /* 撤销只让分配失败，不停止任务：状态仍是 RUNNING。 */
    TT_ASSERT_INT((int)FW_APP_STATE_RUNNING, (int)fw_app_state(g_ctx));
    stop_all();
}

int main(void)
{
    TT_PROGRAM("test_quota");
    TT_RUN(unlimited_quota_allocates_and_accounts);
    TT_RUN(over_quota_is_quota_error_and_counts_denial);
    TT_RUN(freed_bytes_come_back);
    TT_RUN(res_acquire_reports_quota_not_nomem);
    TT_RUN(revoke_blocks_new_allocations_but_does_not_kill);
    return tt_summary();
}
