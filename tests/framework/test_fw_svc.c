/* tests/framework/test_fw_svc.c —— 服务注册表的三条判定（步骤 5.1）。
 *
 * 验的是"没注册 / 注册了但没在跑 / 正常"必须给出三个**不同**的码：
 * 合成一个，调用方就只剩"放弃"这一个选项。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "apps/demo/app_demo.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

#define SVC_STORAGE 1u
#define SVC_DISPLAY 2u

static struct fw_app_ctx *g_owner;

/* 一个"服务 App"：起来就一直等取消，够用来验 owner 状态。 */
static void svc_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        fw_delay_block(20u);
    }
}

static cm_err_t svc_init(app_ctx_t *ctx)
{
    (void)ctx;
    return CM_OK;
}

static void svc_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static const app_desc_t svc_desc = {
    9u, "svc", svc_init, svc_exit, svc_task, 0u, APP_STACK_MIN,
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
    }
    fw_reset();
}

/* 起一个在跑的实例并等它 RUNNING（owner 只有 RUNNING 才算"可用"）。 */
static struct fw_app_ctx *start_owner(uint32_t id)
{
    struct fw_app_ctx *c = fw_app_ctx_new_with_id(&svc_desc, id);

    if (c == NULL) {
        return NULL;
    }
    if (fw_app_start(c) != CM_OK) {
        return NULL;
    }
    for (int i = 0; i < 400; i++) {
        if (fw_app_state(c) == FW_APP_STATE_RUNNING) {
            return c;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    return NULL;
}

static void setup(void)
{
    fw_reset();
    g_owner = NULL;
    (void)fw_platform_bind(pf_host_fake_ops_get());
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
}

TT_CASE(unregistered_service_is_not_supported)
{
    setup();
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_svc_ready(SVC_STORAGE));
    TT_ASSERT_TRUE(fw_svc_owner(SVC_STORAGE) == NULL);
    stop_all();
}

TT_CASE(registered_but_dead_service_is_unavailable)
{
    setup();
    g_owner = start_owner(9u);
    TT_ASSERT_NOT_NULL(g_owner);
    TT_ASSERT_INT(CM_OK, fw_svc_register_ctx(SVC_STORAGE, g_owner));
    TT_ASSERT_INT(CM_OK, fw_svc_ready(SVC_STORAGE)); /* 先确认它本来是好的 */

    /* 停掉 owner：服务还在表里，但不该再"可用" */
    fw_app_request_cancel(g_owner);
    for (int i = 0; i < 400; i++) {
        if (fw_app_state(g_owner) == FW_APP_STATE_STOPPED) {
            break;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT(FW_APP_STATE_STOPPED, fw_app_state(g_owner));
    TT_ASSERT_INT(CM_ERR_UNAVAILABLE, fw_svc_ready(SVC_STORAGE));
    stop_all();
}

TT_CASE(owner_exit_leaves_an_unavailable_marker)
{
    setup();
    g_owner = start_owner(9u);
    TT_ASSERT_NOT_NULL(g_owner);
    TT_ASSERT_INT(CM_OK, fw_svc_register_ctx(SVC_STORAGE, g_owner));
    /* 走退出路径：表项要留着（UNAVAILABLE），owner 指针必须清掉（不能悬垂） */
    fw_app_request_cancel(g_owner);
    for (int i = 0; i < 400; i++) {
        if (fw_app_state(g_owner) == FW_APP_STATE_STOPPED) {
            break;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT(CM_ERR_UNAVAILABLE, fw_svc_ready(SVC_STORAGE));
    TT_ASSERT_TRUE(fw_svc_owner(SVC_STORAGE) == NULL);
    stop_all();
}

/* 崩溃后的重启：新实例重新注册同一个 id，必须成功（这就是"可重试"）。 */
TT_CASE(restarted_owner_can_claim_the_slot_back)
{
    struct fw_app_ctx *second;

    setup();
    g_owner = start_owner(9u);
    TT_ASSERT_NOT_NULL(g_owner);
    TT_ASSERT_INT(CM_OK, fw_svc_register_ctx(SVC_STORAGE, g_owner));
    fw_app_request_cancel(g_owner);
    for (int i = 0; i < 400; i++) {
        if (fw_app_state(g_owner) == FW_APP_STATE_STOPPED) {
            break;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT(CM_ERR_UNAVAILABLE, fw_svc_ready(SVC_STORAGE));

    second = start_owner(10u);
    TT_ASSERT_NOT_NULL(second);
    TT_ASSERT_INT(CM_OK, fw_svc_register_ctx(SVC_STORAGE, second)); /* 认领 */
    TT_ASSERT_INT(CM_OK, fw_svc_ready(SVC_STORAGE));
    TT_ASSERT_TRUE(fw_svc_owner(SVC_STORAGE) == second);
    stop_all();
}

TT_CASE(double_register_is_rejected_not_idempotent)
{
    setup();
    g_owner = start_owner(9u);
    TT_ASSERT_NOT_NULL(g_owner);
    TT_ASSERT_INT(CM_OK, fw_svc_register_ctx(SVC_STORAGE, g_owner));
    TT_ASSERT_INT(CM_ERR_STATE, fw_svc_register_ctx(SVC_STORAGE, g_owner)); /* 同一 owner */
    stop_all();
}

TT_CASE(other_owner_cannot_steal_or_drop)
{
    struct fw_app_ctx *a;
    struct fw_app_ctx *b;

    setup();
    a = start_owner(9u);
    b = start_owner(10u);
    TT_ASSERT_NOT_NULL(a);
    TT_ASSERT_NOT_NULL(b);
    TT_ASSERT_INT(CM_OK, fw_svc_register_ctx(SVC_STORAGE, a));
    TT_ASSERT_INT(CM_ERR_BUSY, fw_svc_register_ctx(SVC_STORAGE, b));
    TT_ASSERT_INT(CM_ERR_STATE, fw_svc_unregister_ctx(SVC_STORAGE, b));
    TT_ASSERT_INT(CM_OK, fw_svc_unregister_ctx(SVC_STORAGE, a));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_svc_ready(SVC_STORAGE));
    stop_all();
}

TT_CASE(bad_arguments_are_rejected)
{
    struct fw_app_ctx *a;

    setup();
    a = start_owner(9u);
    TT_ASSERT_NOT_NULL(a);
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_register_ctx(0u, a)); /* id 0 永远无效 */
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_register_ctx((fw_svc_id_t)FW_SVC_MAX, a));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_register_ctx(SVC_STORAGE, NULL));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_ready(0u));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_svc_unregister_ctx(SVC_DISPLAY, a));
    stop_all();
}

int main(void)
{
    TT_PROGRAM("test_fw_svc");

    TT_RUN(unregistered_service_is_not_supported);
    TT_RUN(registered_but_dead_service_is_unavailable);
    TT_RUN(owner_exit_leaves_an_unavailable_marker);
    TT_RUN(restarted_owner_can_claim_the_slot_back);
    TT_RUN(double_register_is_rejected_not_idempotent);
    TT_RUN(other_owner_cannot_steal_or_drop);
    TT_RUN(bad_arguments_are_rejected);
    return tt_summary();
}
