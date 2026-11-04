/* tests/framework/test_svc_public.c —— App 能不能真的当服务（D44）。
 *
 * 这一条之前是**设计上成立、代码上做不到**：`fw_svc_register()` 在
 * `framework/src/fw_internal.h` 里，App 看不见，所以"一个 App 提供服务"只是文档。
 * 现在公开面在 `api/af_api/svc.h`，owner 由框架自己取（= 当前 App），
 * 于是本文件只 include `api/af_api/app.h` 就能写出一个服务 App。
 *
 * 三条：不在 App 任务里注册 → STATE；在 App 里注册 → 别人查得到 OK；
 *       主动注销 → 回到 NOT_SUPPORTED。
 */
#include "tinytest.h"

#include "api/af_api/app.h"
#include "apps/app_manifest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

#define SVC_PROBE 11u

static fw_app_ctx_t *g_inst;
static int g_registered; /* 跨任务读：一律 __atomic */
static int g_unregistered;

static void probe_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

/* App 任务：注册 → 等取消 → 注销（模拟"我确定不再提供服务了"）。 */
static void probe_task(app_ctx_t *ctx)
{
    (void)ctx;
    __atomic_store_n(&g_registered, (fw_svc_register(SVC_PROBE) == CM_OK) ? 1 : 0,
                     __ATOMIC_RELAXED);
    for (;;) {
        fw_delay_block(20u);
    }
}

static cm_err_t probe_init(app_ctx_t *ctx)
{
    (void)ctx;
    return CM_OK;
}

static const app_desc_t probe_desc = {
    12u, "probe", probe_init, probe_exit, probe_task, 0u, APP_STACK_MIN,
};

static void wait_state(int want_running)
{
    for (int i = 0; i < 400; i++) {
        fw_app_state_e st = fw_app_state(g_inst);
        int running = (st == FW_APP_STATE_RUNNING) ? 1 : 0;

        if (running == want_running &&
            (want_running == 0 || __atomic_load_n(&g_registered, __ATOMIC_RELAXED) != 0)) {
            return;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
}

static void stop_and_reset(void)
{
    if (g_inst != NULL) {
        fw_app_request_cancel(g_inst);
        for (int i = 0; i < 400; i++) {
            fw_app_state_e st = fw_app_state(g_inst);

            if (st == FW_APP_STATE_STOPPED || st == FW_APP_STATE_FAILED) {
                break;
            }
            pf_host_fake_ops_get()->sleep_ms(2u);
        }
        if (fw_live_count() > 0u) {
            fw_live_remove(g_inst);
        }
        fw_app_ctx_free(g_inst);
        g_inst = NULL;
    }
    __atomic_store_n(&g_registered, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_unregistered, 0, __ATOMIC_RELAXED);
    fw_reset();
}

TT_CASE(register_outside_an_app_is_state_error)
{
    TT_ASSERT_INT(CM_ERR_STATE, fw_svc_register(SVC_PROBE));
    TT_ASSERT_INT(CM_ERR_STATE, fw_svc_unregister(SVC_PROBE));
}

TT_CASE(app_registers_itself_and_others_see_ready)
{
    const pf_ops_t *ops = pf_host_fake_ops_get();

    TT_ASSERT_INT(CM_OK, fw_platform_bind(ops));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_svc_ready(SVC_PROBE)); /* 还没注册 */

    g_inst = fw_app_ctx_new_with_id(&probe_desc, 12u);
    TT_ASSERT_TRUE(g_inst != NULL);
    TT_ASSERT_INT(CM_OK, fw_app_start(g_inst));

    wait_state(1);
    TT_ASSERT_INT(1, __atomic_load_n(&g_registered, __ATOMIC_RELAXED));
    TT_ASSERT_INT(CM_OK, fw_svc_ready(SVC_PROBE)); /* 别人按名字找得到 */
    stop_and_reset();
}

TT_CASE(unregister_removes_the_service_entirely)
{
    const pf_ops_t *ops = pf_host_fake_ops_get();

    TT_ASSERT_INT(CM_OK, fw_platform_bind(ops));
    g_inst = fw_app_ctx_new_with_id(&probe_desc, 12u);
    TT_ASSERT_TRUE(g_inst != NULL);
    TT_ASSERT_INT(CM_OK, fw_app_start(g_inst));
    wait_state(1);

    /* 在 App 任务里注销要"当前 App"，这里用测试侧代劳：内部形式传 owner。 */
    TT_ASSERT_INT(CM_OK, fw_svc_unregister_ctx(SVC_PROBE, g_inst));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_svc_ready(SVC_PROBE));
    stop_and_reset();
}

int main(void)
{
    tt_set_program("test_svc_public");
    TT_RUN(register_outside_an_app_is_state_error);
    TT_RUN(app_registers_itself_and_others_see_ready);
    TT_RUN(unregister_removes_the_service_entirely);
    return tt_summary();
}
