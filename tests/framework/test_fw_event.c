/* tests/framework/test_fw_event.c —— 事件（步骤 7.1）。
 *
 * 验四件事：**提交即拷贝**（post 返回后调用方缓冲可重用）、**FIFO 且满了丢新的**、
 * **未声明主题是 NOT_SUPPORTED 而不是崩**、**wait 是伪阻塞点**（超时返回 TIMEOUT、
 * 等待期间被取消会走退出路径且不再返回）。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

#define T_BUTTON 1u
#define T_PAYLOAD 8u

static fw_app_ctx_t *g_ctx;
static int g_scenario;
static int g_wait_rc;
static int g_after_wait_ran; /* wait 之后那行有没有执行（取消场景必须为 0） */
static unsigned char g_seen[T_PAYLOAD];
static unsigned char g_payload[T_PAYLOAD];
static unsigned char g_out[T_PAYLOAD];
static size_t g_len;

/* 跨任务观测点一律走原子：volatile 不给 happens-before（D20 的教训）。 */
static int flag_get(int *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void flag_set(int *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void event_task(app_ctx_t *ctx)
{
    unsigned char buf[T_PAYLOAD];
    size_t len = 0u;

    (void)ctx;
    switch (g_scenario) {
    case 0: /* 等一个永远不会来的事件：必须超时，且不能挂住 */
        flag_set(&g_wait_rc, (int)fw_event_wait(T_BUTTON, buf, sizeof(buf), &len, 50u));
        break;
    case 1: /* 等主线程 30ms 后投递的事件 */
    {
        int rc = (int)fw_event_wait(T_BUTTON, buf, sizeof(buf), &len, 3000u);

        if (rc == CM_OK) {
            for (size_t i = 0; i < T_PAYLOAD; i++) {
                g_seen[i] = buf[i];
            }
        }
        flag_set(&g_wait_rc, rc);
    } break;
    case 2: /* 长等，等主线程取消 */
        (void)fw_event_wait(T_BUTTON, buf, sizeof(buf), &len, 5000u);
        flag_set(&g_after_wait_ran, 1);
        break;
    default:
        break;
    }
    for (;;) {
        fw_delay_block(20u);
    }
}

static cm_err_t idle_init(app_ctx_t *ctx)
{
    (void)ctx;
    return CM_OK;
}

static void idle_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static const app_desc_t waiter_desc = {
    50u, "waiter", idle_init, idle_exit, event_task, 0u, APP_STACK_MIN,
};

static void stop_all(void)
{
    /* 直接实例不在 live 表里（没走 fw_boot），必须显式停它；
     * 否则下一轮的 fw_reset() 会清掉一个还有任务在等的主题队列。 */
    if (g_ctx != NULL) {
        fw_app_request_cancel(g_ctx);
        for (int i = 0; i < 800; i++) {
            if (fw_app_state(g_ctx) == FW_APP_STATE_STOPPED ||
                fw_app_state(g_ctx) == FW_APP_STATE_FAILED) {
                break;
            }
            pf_host_fake_ops_get()->sleep_ms(2u);
        }
        pf_host_fake_ops_get()->sleep_ms(50u); /* 让线程真的走完 pthread_exit */
        fw_app_ctx_free(g_ctx);
        g_ctx = NULL;
    }
    while (fw_live_count() > 0u) {
        fw_app_ctx_t *c = fw_live_at(0u);

        fw_app_request_cancel(c);
        for (int i = 0; i < 800; i++) {
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
    g_wait_rc = 0;
    g_after_wait_ran = 0;
    g_len = 0u;
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        g_seen[i] = 0u;
        g_out[i] = 0u;
    }
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
}

static fw_app_ctx_t *start_waiter(void)
{
    fw_app_ctx_t *c = fw_app_ctx_new_with_id(&waiter_desc, 50u);

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

/* 1. 声明 → post → take：负载原样回来，长度对。 */
TT_CASE(declared_topic_round_trips_the_payload)
{
    setup(0);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        g_payload[i] = (unsigned char)(0x10u + i);
    }
    TT_ASSERT_INT(CM_OK, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD));
    TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, g_out, sizeof(g_out), &g_len));
    TT_ASSERT_INT((int)T_PAYLOAD, (int)g_len);
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        TT_ASSERT_INT((int)g_payload[i], (int)g_out[i]);
    }
    stop_all();
}

/* 2. 提交即拷贝：post 返回后改原缓冲，队列里那份不受影响。 */
TT_CASE(post_copies_so_the_caller_buffer_is_reusable)
{
    setup(0);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        g_payload[i] = 0xAAu;
    }
    TT_ASSERT_INT(CM_OK, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD));
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        g_payload[i] = 0xFFu; /* 下一次中断会这么干 */
    }
    TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, g_out, sizeof(g_out), &g_len));
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        TT_ASSERT_INT(0xAA, (int)g_out[i]);
    }
    stop_all();
}

/* 3. 没声明的主题：post / isr post / take / wait 全是 NOT_SUPPORTED，不许崩。 */
TT_CASE(undeclared_topic_is_not_supported_everywhere)
{
    setup(0);
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_event_post_isr(T_BUTTON, g_payload));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_event_take(T_BUTTON, g_out, sizeof(g_out), &g_len));
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, fw_event_wait(T_BUTTON, g_out, sizeof(g_out), &g_len, 10u));
    stop_all();
}

/* 4. 声明本身的边界：0 号、超大、重复声明。 */
TT_CASE(declare_rejects_bad_topics_and_sizes)
{
    setup(0);
    TT_ASSERT_INT(CM_ERR_INVALID, fw_event_declare(0u, T_PAYLOAD));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_event_declare(T_BUTTON, 0u));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_event_declare(T_BUTTON, FW_EVENT_MAX_BYTES + 1u));
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    TT_ASSERT_INT(CM_ERR_BUSY, fw_event_declare(T_BUTTON, T_PAYLOAD));
    stop_all();
}

/* 5. FIFO + 满：深度 4，第 5 个返回 BUSY，先来的四个顺序不变。 */
TT_CASE(queue_is_fifo_and_drops_the_newest_when_full)
{
    setup(0);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    for (unsigned char k = 0u; k < FW_EVENT_DEPTH; k++) {
        for (size_t i = 0; i < T_PAYLOAD; i++) {
            g_payload[i] = k;
        }
        TT_ASSERT_INT(CM_OK, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD));
    }
    TT_ASSERT_INT(CM_ERR_BUSY, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD));
    for (unsigned char k = 0u; k < FW_EVENT_DEPTH; k++) {
        TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, g_out, sizeof(g_out), &g_len));
        TT_ASSERT_INT((int)k, (int)g_out[0]);
    }
    TT_ASSERT_INT(CM_ERR_TIMEOUT, fw_event_take(T_BUTTON, g_out, sizeof(g_out), &g_len));
    stop_all();
}

/* 6. 长度/容量不符：明确 INVALID，不静默截断。 */
TT_CASE(length_and_capacity_are_checked)
{
    setup(0);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD - 1u));
    TT_ASSERT_INT(CM_OK, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD));
    TT_ASSERT_INT(CM_ERR_INVALID, fw_event_take(T_BUTTON, g_out, T_PAYLOAD - 1u, &g_len));
    TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, g_out, sizeof(g_out), &g_len));
    stop_all();
}

/* 7. ISR 版：长度由声明决定，能取到。 */
TT_CASE(isr_post_uses_the_declared_length)
{
    setup(0);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        g_payload[i] = 0x5Au;
    }
    TT_ASSERT_INT(CM_OK, fw_event_post_isr(T_BUTTON, g_payload));
    TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, g_out, sizeof(g_out), &g_len));
    TT_ASSERT_INT((int)T_PAYLOAD, (int)g_len);
    TT_ASSERT_INT(0x5A, (int)g_out[0]);
    stop_all();
}

/* 8. wait 超时：50ms 内没有事件就返回 TIMEOUT，不许挂死。 */
TT_CASE(wait_times_out_without_an_event)
{
    setup(0);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    g_ctx = start_waiter();
    TT_ASSERT_NOT_NULL(g_ctx);
    for (int i = 0; i < 800 && flag_get(&g_wait_rc) == 0; i++) {
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT(CM_ERR_TIMEOUT, flag_get(&g_wait_rc));
    TT_ASSERT_INT(0, flag_get(&g_after_wait_ran));
    stop_all();
}

/* 9. wait 拿到事件：主线程投递，App 任务里等到。 */
TT_CASE(wait_returns_the_posted_event)
{
    setup(1);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        g_payload[i] = (unsigned char)(0xA0u + i);
    }
    g_ctx = start_waiter();
    TT_ASSERT_NOT_NULL(g_ctx);
    pf_host_fake_ops_get()->sleep_ms(30u);
    TT_ASSERT_INT(CM_OK, fw_event_post(T_BUTTON, g_payload, T_PAYLOAD));
    for (int i = 0; i < 800 && flag_get(&g_wait_rc) == 0; i++) {
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT(CM_OK, flag_get(&g_wait_rc));
    for (size_t i = 0; i < T_PAYLOAD; i++) {
        TT_ASSERT_INT((int)g_payload[i], (int)g_seen[i]);
    }
    stop_all();
}

/* 10. wait 期间被取消：走退出路径，wait 之后那行不执行。 */
TT_CASE(wait_is_a_cancellation_point)
{
    setup(2);
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, T_PAYLOAD));
    g_ctx = start_waiter();
    TT_ASSERT_NOT_NULL(g_ctx);
    pf_host_fake_ops_get()->sleep_ms(30u);
    fw_app_request_cancel(g_ctx);
    for (int i = 0; i < 800; i++) {
        if (fw_app_state(g_ctx) == FW_APP_STATE_STOPPED) {
            break;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT(FW_APP_STATE_STOPPED, (int)fw_app_state(g_ctx));
    TT_ASSERT_INT(0, flag_get(&g_after_wait_ran));
    TT_ASSERT_INT(1, (int)fw_app_exit_calls(g_ctx));
    stop_all();
}

int main(void)
{
    tt_set_program("test_fw_event");

    TT_RUN(declared_topic_round_trips_the_payload);
    TT_RUN(post_copies_so_the_caller_buffer_is_reusable);
    TT_RUN(undeclared_topic_is_not_supported_everywhere);
    TT_RUN(declare_rejects_bad_topics_and_sizes);
    TT_RUN(queue_is_fifo_and_drops_the_newest_when_full);
    TT_RUN(length_and_capacity_are_checked);
    TT_RUN(isr_post_uses_the_declared_length);
    TT_RUN(wait_times_out_without_an_event);
    TT_RUN(wait_returns_the_posted_event);
    TT_RUN(wait_is_a_cancellation_point);

    return tt_summary();
}
