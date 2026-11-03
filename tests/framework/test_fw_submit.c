/* tests/framework/test_fw_submit.c —— 提交型接口（步骤 5.2）。
 *
 * 这个文件要证明的不是"能提交"，而是**提交之后调用方的缓冲区就自由了**：
 * 拷贝发生在 `fw_svc_submit()` 里，而不是"以后某时刻"。第 3 条用例是核心
 * ——提交完立刻把源缓冲区涂黑，取出来的必须还是原内容。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

#define SVC_FRAME 5u

static struct fw_app_ctx *g_owner;
static uint32_t g_done_key;
static int g_done_superseded;
static int g_done_calls;

static void on_done(uint32_t key, int superseded, void *user)
{
    (void)user;
    __atomic_store_n(&g_done_key, key, __ATOMIC_RELAXED);
    __atomic_store_n(&g_done_superseded, superseded, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_done_calls, 1, __ATOMIC_RELAXED);
}

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
    11u, "framesvc", svc_init, svc_exit, svc_task, 0u, APP_STACK_MIN,
};

static void stop_all(void)
{
    while (fw_live_count() > 0u) {
        struct fw_app_ctx *c = fw_live_at(0u);

        fw_app_request_cancel(c);
        for (int i = 0; i < 400; i++) {
            if (fw_app_state(c) == FW_APP_STATE_STOPPED ||
                fw_app_state(c) == FW_APP_STATE_FAILED) {
                break;
            }
            pf_host_fake_ops_get()->sleep_ms(2u);
        }
        fw_live_remove(c);
    }
    fw_reset();
}

static struct fw_app_ctx *start_owner(uint32_t id)
{
    struct fw_app_ctx *c = fw_app_ctx_new_with_id(&svc_desc, id);

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

/* 起服务（在跑）+ 注册。返回前所有状态都是确定的。 */
static void setup_registered(void)
{
    fw_reset();
    g_owner = NULL;
    __atomic_store_n(&g_done_key, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_done_superseded, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_done_calls, 0, __ATOMIC_RELAXED);
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
    g_owner = start_owner(20u);
    TT_ASSERT_NOT_NULL(g_owner);
    TT_ASSERT_INT(CM_OK, fw_svc_register(SVC_FRAME, g_owner));
}

TT_CASE(submit_to_unregistered_service_is_not_supported)
{
    fw_reset();
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_svc_submit(SVC_FRAME, 1u, "x", 1u, NULL, NULL));
    stop_all();
}

TT_CASE(caller_buffer_is_reusable_right_after_submit)
{
    unsigned char src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char out[8] = {0};
    size_t got = 0u;
    uint32_t key = 0u;

    setup_registered();
    TT_ASSERT_INT(CM_OK, fw_svc_submit(SVC_FRAME, 42u, src, sizeof src, on_done, NULL));

    /* 提交返回的那一刻，调用方就可以乱改这块内存了 —— 这是 D24 的全部意思。 */
    for (int i = 0; i < (int)sizeof src; i++) {
        src[i] = 0xFFu;
    }

    TT_ASSERT_INT(CM_OK, fw_svc_submit_take(SVC_FRAME, &key, out, sizeof out, &got));
    TT_ASSERT_INT(42, (int)key);
    TT_ASSERT_INT(8, (int)got);
    for (int i = 1; i <= 8; i++) {
        TT_ASSERT_INT(i, (int)out[i - 1]);
    }
    stop_all();
}

TT_CASE(take_fires_completion)
{
    unsigned char out[4] = {0};
    size_t got = 0u;

    setup_registered();
    TT_ASSERT_INT(CM_OK, fw_svc_submit(SVC_FRAME, 7u, "abcd", 4u, on_done, NULL));
    TT_ASSERT_INT(0, __atomic_load_n(&g_done_calls, __ATOMIC_RELAXED));
    TT_ASSERT_INT(CM_OK, fw_svc_submit_take(SVC_FRAME, NULL, out, sizeof out, &got));
    TT_ASSERT_INT(1, __atomic_load_n(&g_done_calls, __ATOMIC_RELAXED));
    TT_ASSERT_INT(0, __atomic_load_n(&g_done_superseded, __ATOMIC_RELAXED));
    TT_ASSERT_INT(7, (int)__atomic_load_n(&g_done_key, __ATOMIC_RELAXED));
    stop_all();
}

TT_CASE(same_key_overwrites_the_pending_frame)
{
    unsigned char out[8] = {0};
    size_t got = 0u;
    uint32_t key = 0u;

    setup_registered();
    TT_ASSERT_INT(CM_OK, fw_svc_submit(SVC_FRAME, 9u, "old!", 4u, on_done, NULL));
    TT_ASSERT_INT(CM_OK, fw_svc_submit(SVC_FRAME, 9u, "new!", 4u, on_done, NULL));

    /* 第一帧没被消费，是被顶掉的 —— 它必须收到通知，否则等它的调用方永远等。 */
    TT_ASSERT_INT(1, __atomic_load_n(&g_done_calls, __ATOMIC_RELAXED));
    TT_ASSERT_INT(1, __atomic_load_n(&g_done_superseded, __ATOMIC_RELAXED));
    TT_ASSERT_INT(1, (int)fw_svc_submit_count(SVC_FRAME));

    TT_ASSERT_INT(CM_OK, fw_svc_submit_take(SVC_FRAME, &key, out, sizeof out, &got));
    TT_ASSERT_STR("new!", (const char *)out);
    TT_ASSERT_INT(2, __atomic_load_n(&g_done_calls, __ATOMIC_RELAXED));
    stop_all();
}

TT_CASE(different_keys_are_both_kept)
{
    unsigned char out[8] = {0};
    size_t got = 0u;
    uint32_t key = 0u;

    setup_registered();
    TT_ASSERT_INT(CM_OK, fw_svc_submit(SVC_FRAME, 1u, "aa", 2u, NULL, NULL));
    TT_ASSERT_INT(CM_OK, fw_svc_submit(SVC_FRAME, 2u, "bb", 2u, NULL, NULL));
    TT_ASSERT_INT(2, (int)fw_svc_submit_count(SVC_FRAME));

    TT_ASSERT_INT(CM_OK, fw_svc_submit_take(SVC_FRAME, &key, out, sizeof out, &got));
    TT_ASSERT_INT(1, (int)key);
    TT_ASSERT_INT(CM_OK, fw_svc_submit_take(SVC_FRAME, &key, out, sizeof out, &got));
    TT_ASSERT_INT(2, (int)key);
    /* 取空之后不是"还有 0 帧可用"，而是明确说没有 —— 空队列不是错误。 */
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED,
                  fw_svc_submit_take(SVC_FRAME, &key, out, sizeof out, &got));
    stop_all();
}

TT_CASE(small_capacity_leaves_the_frame_in_place)
{
    unsigned char out[1] = {0};
    size_t got = 0u;

    setup_registered();
    TT_ASSERT_INT(CM_OK, fw_svc_submit(SVC_FRAME, 3u, "hello", 5u, NULL, NULL));
    /* 容量不够时不消费：调用方扩容再来一次即可，不需要重新提交。 */
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_submit_take(SVC_FRAME, NULL, out, sizeof out, &got));
    TT_ASSERT_INT(5, (int)got);
    TT_ASSERT_INT(1, (int)fw_svc_submit_count(SVC_FRAME));
    stop_all();
}

TT_CASE(bad_arguments_are_rejected)
{
    setup_registered();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_submit(SVC_FRAME, 0u, NULL, 4u, NULL, NULL));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_submit(SVC_FRAME, 0u, "x", 0u, NULL, NULL));
    TT_ASSERT_INT(CM_ERR_INVALID,
                  fw_svc_submit(SVC_FRAME, 0u, "x", FW_SUBMIT_MAX_BYTES + 1u, NULL, NULL));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_svc_submit(0u, 0u, "x", 1u, NULL, NULL));
    TT_ASSERT_INT(0, (int)fw_svc_submit_count(SVC_FRAME));
    stop_all();
}

int main(void)
{
    TT_PROGRAM("test_fw_submit");
    TT_RUN(caller_buffer_is_reusable_right_after_submit);
    TT_RUN(take_fires_completion);
    TT_RUN(same_key_overwrites_the_pending_frame);
    TT_RUN(different_keys_are_both_kept);
    TT_RUN(small_capacity_leaves_the_frame_in_place);
    TT_RUN(bad_arguments_are_rejected);
    TT_RUN(submit_to_unregistered_service_is_not_supported);
    return tt_summary();
}
