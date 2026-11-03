/* tests/framework/test_heartbeat.c —— 心跳（步骤 8.1 / 8.2）。
 *
 * 验三件事：**让出点会自动喂**（`fw_delay_block` / `fw_event_wait` / `fw_svc_submit`）、
 * **静默的 App 能被判为卡住**、**判定只在 RUNNING 且阈值非 0 时成立**。
 *
 * 最后一条用例故意留一个纯死循环的 App 不收：它本来就不检查取消位，
 * 框架停不掉它——这正是 8.2 要发现的那种目标，也是"只能重启整机"的那一类。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

static struct fw_app_ctx *g_ctx;
static uint32_t g_spin;

static cm_err_t ok_init(app_ctx_t *ctx)
{
    (void)ctx;
    return CM_OK;
}

static void ok_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

/* 正常 App：每 5ms 走一次可取消的等待。 */
static void beep_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        fw_delay_block(5u);
    }
}

/* 死循环 App：不调任何框架接口。它永远不会喂心跳，也不会检查取消位。 */
static void silent_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        (void)__atomic_add_fetch(&g_spin, 1u, __ATOMIC_RELAXED);
    }
}

static const app_desc_t beep_desc = {
    60u, "beeper", ok_init, ok_exit, beep_task, 0u, APP_STACK_MIN,
};

static const app_desc_t silent_desc = {
    61u, "silent", ok_init, ok_exit, silent_task, 0u, APP_STACK_MIN,
};

static void stop_all(void)
{
    if (g_ctx != NULL) {
        fw_app_request_cancel(g_ctx);
        for (int i = 0; i < 800; i++) {
            if (fw_app_state(g_ctx) == FW_APP_STATE_STOPPED ||
                fw_app_state(g_ctx) == FW_APP_STATE_FAILED) {
                break;
            }
            pf_host_fake_ops_get()->sleep_ms(2u);
        }
        pf_host_fake_ops_get()->sleep_ms(50u);
        fw_app_ctx_free(g_ctx);
        g_ctx = NULL;
    }
    while (fw_live_count() > 0u) {
        struct fw_app_ctx *c = fw_live_at(0u);

        fw_app_request_cancel(c);
        for (int i = 0; i < 800; i++) {
            if (fw_app_state(c) == FW_APP_STATE_STOPPED ||
                fw_app_state(c) == FW_APP_STATE_FAILED) {
                break;
            }
            pf_host_fake_ops_get()->sleep_ms(2u);
        }
        fw_live_remove(c);
        fw_app_ctx_free(c);
    }
    fw_reset();
}

static void setup(void)
{
    fw_reset();
    g_ctx = NULL;
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
}

static struct fw_app_ctx *start(const app_desc_t *d, uint32_t id)
{
    struct fw_app_ctx *c = fw_app_ctx_new_with_id(d, id);

    if (c == NULL || fw_app_start(c) != CM_OK) {
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

/* 1. 正常 App：心跳序号会涨，age 是"刚刚"的量级。 */
TT_CASE(heartbeat_advances_at_yield_points)
{
    uint32_t seq1 = 0u;
    uint32_t seq2 = 0u;
    uint32_t age = 0u;

    setup();
    g_ctx = start(&beep_desc, 60u);
    TT_ASSERT_NOT_NULL(g_ctx);
    pf_host_fake_ops_get()->sleep_ms(30u);
    fw_app_heartbeat(&g_ctx->pub, &seq1, &age);
    TT_ASSERT_TRUE(seq1 > 0u);
    TT_ASSERT_TRUE(age < 60u);
    pf_host_fake_ops_get()->sleep_ms(30u);
    fw_app_heartbeat(&g_ctx->pub, &seq2, NULL);
    TT_ASSERT_TRUE(seq2 > seq1);
    stop_all();
}

/* 2. 正常 App 不该被判卡住。 */
TT_CASE(running_app_is_not_stalled)
{
    setup();
    g_ctx = start(&beep_desc, 60u);
    TT_ASSERT_NOT_NULL(g_ctx);
    pf_host_fake_ops_get()->sleep_ms(30u);
    TT_ASSERT_INT(0, fw_app_is_stalled(&g_ctx->pub, 100u));
    stop_all();
}

/* 3. 阈值 0 / NULL / 没在跑：都不该判卡住（判定只在能判断的时候成立）。 */
TT_CASE(stall_check_is_conservative)
{
    setup();
    TT_ASSERT_INT(0, fw_app_is_stalled(NULL, 50u));
    g_ctx = start(&beep_desc, 60u);
    TT_ASSERT_NOT_NULL(g_ctx);
    TT_ASSERT_INT(0, fw_app_is_stalled(&g_ctx->pub, 0u));
    stop_all();
    TT_ASSERT_INT(0, fw_app_is_stalled(&g_ctx->pub, 50u)); /* 已释放的上下文不再判定 */
}

/* 4. 纯死循环：心跳不动 → 判为卡住；同时它自己那个取消位形同虚设。 */
TT_CASE(silent_busy_loop_is_detected_but_cannot_be_stopped)
{
    uint32_t seq = 123u;
    uint32_t age = 0u;

    setup();
    g_ctx = start(&silent_desc, 61u);
    TT_ASSERT_NOT_NULL(g_ctx);
    pf_host_fake_ops_get()->sleep_ms(80u);
    fw_app_heartbeat(&g_ctx->pub, &seq, &age);
    TT_ASSERT_INT(0, (int)seq);          /* 一次都没喂过 */
    TT_ASSERT_INT(1, fw_app_is_stalled(&g_ctx->pub, 50u));
    /* 注意：这里**不停**它——发了取消请求它也不理，这正是"只能重启整机"的那类。 */
    g_ctx = NULL;
}

int main(void)
{
    tt_set_program("test_heartbeat");

    TT_RUN(heartbeat_advances_at_yield_points);
    TT_RUN(running_app_is_not_stalled);
    TT_RUN(stall_check_is_conservative);
    TT_RUN(silent_busy_loop_is_detected_but_cannot_be_stopped);

    return tt_summary();
}
