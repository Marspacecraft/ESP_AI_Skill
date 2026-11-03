/* tests/framework/test_fw_embed.c —— 给"别人的 app_main"用的启动点。
 *
 * 这里验的不是"顺序对不对"（那是 `test_fw_main.c` 的事，两边共用 `fw_startup()`），
 * 而是嵌入语义里独有的三条：**只能成功一次**、**失败不烧掉那次机会**、
 * **`fw_reset()` 之后回到没启动过的状态**。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "apps/demo/app_demo.h"
#include "framework/include/fw_embed.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

static void stop_all(void)
{
    while (fw_live_count() > 0u) {
        struct fw_app_ctx *c = fw_live_at(0u);

        fw_app_request_cancel(c);
        for (int i = 0; i < 200; i++) {
            fw_app_state_e st = fw_app_state(c);

            if (st == FW_APP_STATE_STOPPED || st == FW_APP_STATE_FAILED) {
                break;
            }
            pf_host_fake_ops_get()->sleep_ms(5u);
        }
        fw_live_remove(c);
        fw_app_ctx_free(c);
    }
    fw_reset();
}

/* init() 跑在 App 自己的任务里，启动返回时它可能还没被调度。 */
static int wait_inits(uint32_t want, uint32_t timeout_ms)
{
    const pf_ops_t *real = pf_host_fake_ops_get();
    uint32_t t0 = real->clock_ms();

    while ((uint32_t)(real->clock_ms() - t0) < timeout_ms) {
        if (app_demo_inits() >= want) {
            return 1;
        }
        real->sleep_ms(5u);
    }
    return app_demo_inits() >= want;
}

TT_CASE(null_manifest_is_rejected)
{
    TT_ASSERT_INT(CM_ERR_INVALID, fw_embed_start(NULL));
}

TT_CASE(embed_starts_the_system_app)
{
    TT_ASSERT_INT(CM_OK, fw_embed_start(&apps_manifest));
    TT_ASSERT_INT(1u, fw_live_count());
    TT_ASSERT_TRUE(wait_inits(1u, 500u));
    stop_all();
}

TT_CASE(second_start_is_rejected)
{
    TT_ASSERT_INT(CM_OK, fw_embed_start(&apps_manifest));
    TT_ASSERT_INT(CM_ERR_STATE, fw_embed_start(&apps_manifest));
    stop_all();
}

TT_CASE(failed_start_does_not_burn_the_only_shot)
{
    /* 清单里放一条 `stack_bytes` 不够的 App：启动必须拒，但**不能**把
     * "已启动过"的标记烧掉——否则宿主改对参数后再也没有机会。 */
    app_desc_t bad = app_demo_desc;
    const app_desc_t *bad_apps[1];
    fw_manifest_t bad_man;

    bad.stack_bytes = APP_STACK_MIN - 1u;
    bad_apps[0] = &bad;
    bad_man.apps = bad_apps;
    bad_man.count = 1u;

    TT_ASSERT_INT(CM_ERR_INVALID, fw_embed_start(&bad_man));
    TT_ASSERT_INT(0u, fw_live_count());

    TT_ASSERT_INT(CM_OK, fw_embed_start(&apps_manifest));
    TT_ASSERT_INT(1u, fw_live_count());
    stop_all();
}

TT_CASE(reset_makes_it_startable_again)
{
    TT_ASSERT_INT(CM_OK, fw_embed_start(&apps_manifest));
    stop_all(); /* 里面会 fw_reset() */
    TT_ASSERT_INT(CM_OK, fw_embed_start(&apps_manifest));
    TT_ASSERT_INT(1u, fw_live_count());
    stop_all();
}

int main(void)
{
    TT_PROGRAM("test_fw_embed");
    TT_RUN(null_manifest_is_rejected);
    TT_RUN(embed_starts_the_system_app);
    TT_RUN(second_start_is_rejected);
    TT_RUN(failed_start_does_not_burn_the_only_shot);
    TT_RUN(reset_makes_it_startable_again);
    return tt_summary();
}
