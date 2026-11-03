/* tests/framework/test_fw_main.c —— 启动顺序（步骤 4.1）。
 *
 * `app_main()` 里有永久阻塞，测不了；能测的是它前面那段 `fw_startup()`：
 * bind → init → boot。所以这里验的是"顺序与失败码"，不是"能不能一直跑下去"。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"
#include "apps/demo/app_demo.h"

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
    }
    fw_reset();
}

TT_CASE(null_ops_is_rejected)
{
    TT_ASSERT_INT(CM_ERR_INVALID, fw_startup(NULL, &apps_manifest));
}

TT_CASE(null_manifest_is_rejected)
{
    TT_ASSERT_INT(CM_ERR_INVALID, fw_startup(pf_host_fake_ops_get(), NULL));
}

TT_CASE(wrong_version_is_rejected_before_anything_starts)
{
    pf_ops_t bad = *pf_host_fake_ops_get();

    bad.version = PF_OPS_VERSION + 1u;
    TT_ASSERT_INT(CM_ERR_VERSION, fw_startup(&bad, &apps_manifest));
    /* 拒绝之后不能留下半启动：库里没有实例、也没有绑上表 */
    TT_ASSERT_INT(0u, fw_live_count());
    TT_ASSERT_INT(CM_ERR_STATE, fw_boot()); /* 未 init → 不能 boot */
    fw_reset();
}

/* init() 跑在 App 自己的任务里，boot 返回时它可能还没被调度——
 * 所以这里必须等，不能"boot 返回就断言 init 过了"。 */
static int wait_inits(uint32_t want, uint32_t timeout_ms)
{
    const pf_ops_t *real = pf_host_fake_ops_get();
    uint32_t t0 = real->clock_ms();

    while (app_demo_inits() < want) {
        if ((uint32_t)(real->clock_ms() - t0) > timeout_ms) {
            return 0;
        }
        real->sleep_ms(1u);
    }
    return 1;
}

TT_CASE(startup_brings_up_the_system_app)
{
    TT_ASSERT_INT(CM_OK, fw_startup(pf_host_fake_ops_get(), &apps_manifest));
    TT_ASSERT_INT(1u, fw_live_count()); /* 表在 boot 里就填好了，不等任务 */
    TT_ASSERT_TRUE(wait_inits(1u, 2000u));
    TT_ASSERT_INT(2u, fw_live_at(0u)->pub.id); /* 清单里的 id 原样带进上下文 */
    stop_all();
}

TT_CASE(startup_is_one_shot)
{
    cm_err_t first = fw_startup(pf_host_fake_ops_get(), &apps_manifest);
    cm_err_t second = fw_startup(pf_host_fake_ops_get(), &apps_manifest);

    stop_all(); /* 先收拾现场，再断言：断言失败提前返回时不许污染下一条用例 */
    TT_ASSERT_INT(CM_OK, first);
    TT_ASSERT_INT(CM_ERR_STATE, second);
}

int main(void)
{
    TT_PROGRAM("test_fw_main");

    TT_RUN(null_ops_is_rejected);
    TT_RUN(null_manifest_is_rejected);
    TT_RUN(wrong_version_is_rejected_before_anything_starts);
    TT_RUN(startup_brings_up_the_system_app);
    TT_RUN(startup_is_one_shot);
    return tt_summary();
}
