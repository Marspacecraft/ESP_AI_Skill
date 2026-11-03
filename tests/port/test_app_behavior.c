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

#include "api/af_api/cap.h"
#include "api/af_api/uart.h"
#include "framework/include/fw_app.h"
#include "framework/include/fw_embed.h"
#include "framework/src/fw_internal.h"
#include "platform/port/pf_ops.h"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <time.h>
#endif

/* ------------------------------------------------------------------ */
/* 观测点：跨任务读写，全部走原子（否则 TSan 与真机都可能给出假绿）        */
/* ------------------------------------------------------------------ */

static int g_init_calls;
static int g_exit_calls;
static int g_ticks;

typedef struct {
    uint32_t key;
    int superseded;
} done_ev_t;

static done_ev_t g_ev[8];
static int g_ev_n;
static int g_done_calls;
static int g_done_superseded;
static uint32_t g_done_key;
#define SVC_TEST_ID 11u /* 编译期常量：跨任务共享的东西不设可变的 */
static const uint8_t g_svc_id = SVC_TEST_ID;

static int obs_get(const int *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void obs_add(int *p)
{
    (void)__atomic_add_fetch(p, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* 被测 App                                                           */
/* ------------------------------------------------------------------ */

static cm_err_t ok_init(app_ctx_t *ctx)
{
    obs_add(&g_init_calls);
    ctx->slots[0] = ctx;
    return CM_OK;
}
static void ok_exit(app_ctx_t *ctx)
{
    (void)ctx;
    obs_add(&g_exit_calls);
}
static void ok_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        obs_add(&g_ticks);
        fw_delay_block(20u);
    }
}

static cm_err_t bad_init(app_ctx_t *ctx)
{
    (void)ctx;
    obs_add(&g_init_calls);
    return CM_ERR_INVALID;
}

/* "非框架循环"：**不**调任何 `fw_*` 阻塞接口，所以心跳一次都不会被喂。
 * 这就是 D29 说的那一类——框架能发现它，但没有办法让它自己停下。
 *
 * 为什么每轮让出 1 tick（`vTaskDelay(1)` / `nanosleep`）而不是空转：
 * **真·空转在板子上会把优先级更低的测试任务饿死**——第一次上机就是
 * `task_wdt: Task watchdog got triggered` 连打、`PASS` 一行都没出来，
 * 观测者自己先停了。这个现象本身就是 D29 的证据：纯死循环在现场的
 * 表现不是"App 卡住"，而是"整机没法被任何软件手段观测或停下"。
 * 让出一 tick 之后，我们还能观测到"心跳没喂 + 取消位无效"这两条。 */
static int g_spin;

static void spin_yield(void)
{
#if defined(ESP_PLATFORM)
    vTaskDelay(1);
#else
    {
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = 1000000L;
        (void)nanosleep(&ts, NULL);
    }
#endif
}

static void spin_task(app_ctx_t *ctx)
{
    (void)ctx;
    while (obs_get(&g_spin)) {
        obs_add(&g_ticks);
        spin_yield();
    }
}

/* 服务宿主：`init()` 里把服务 11 登记到自己名下，任务只负责转圈。
 * 提交与取走都由主任务做——`fw_svc_submit*` 不要求调用者在 App 任务里，
 * 这样取帧时机是确定的，不用在两条任务之间对齐节奏。 */
static cm_err_t svc_init(app_ctx_t *ctx)
{
    (void)ctx;
    obs_add(&g_init_calls);
    return fw_svc_register_ctx(SVC_TEST_ID, fw_current_ctx());
}

static void svc_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        obs_add(&g_ticks);
        fw_delay_block(20u);
    }
}

/* 一次要 1000 ms 的延时：用来证明"切片"真的把长等待切碎了。 */
static void slow_task(app_ctx_t *ctx)
{
    (void)ctx;
    obs_add(&g_ticks);
    fw_delay_block(1000u);
    obs_add(&g_ticks);
}

static const app_desc_t ok_desc = {
    .id = 40u,
    .name = "beh",
    .init = ok_init,
    .exit = ok_exit,
    .task = ok_task,
    .stack_bytes = APP_STACK_MIN,
    .flags = APP_FLAG_SYSTEM,
};
static const app_desc_t bad_desc = {
    .id = 41u,
    .name = "beh_bad",
    .init = bad_init,
    .exit = ok_exit,
    .task = ok_task,
    .stack_bytes = APP_STACK_MIN,
    .flags = APP_FLAG_SYSTEM,
};
static const app_desc_t spin_desc = {
    .id = 43u,
    .name = "beh_spin",
    .init = ok_init,
    .exit = ok_exit,
    .task = spin_task,
    .stack_bytes = APP_STACK_MIN,
    .flags = APP_FLAG_SYSTEM,
};
static const app_desc_t svc_desc = {
    .id = 44u,
    .name = "beh_svc",
    .init = svc_init,
    .exit = ok_exit,
    .task = svc_task,
    .stack_bytes = APP_STACK_MIN,
    .flags = APP_FLAG_SYSTEM,
};
static const app_desc_t slow_desc = {
    .id = 42u,
    .name = "beh_slow",
    .init = ok_init,
    .exit = ok_exit,
    .task = slow_task,
    .stack_bytes = APP_STACK_MIN,
    .flags = APP_FLAG_SYSTEM,
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
    __atomic_store_n(&g_done_calls, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_done_superseded, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_done_key, 0u, __ATOMIC_RELEASE);
    g_ev_n = 0;
}

static uint32_t now_ms(void)
{
    return pf_platform_ops()->clock_ms();
}

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

/* ------------------------------------------------------------------ */
/* 服务层：注册表 + 提交型接口（清单第 5/6 条的真机证据）                */
/* ------------------------------------------------------------------ */

static void on_done(uint32_t key, int superseded, void *user)
{
    (void)user;
    int n = __atomic_fetch_add(&g_ev_n, 1, __ATOMIC_RELAXED);

    if (n < 8) {
        g_ev[n].key = key;
        g_ev[n].superseded = superseded;
    }
    obs_add(&g_done_calls);
    if (superseded) {
        __atomic_store_n(&g_done_superseded, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_done_key, key, __ATOMIC_RELEASE);
}

static int start_service_host(void)
{
    fw_manifest_t m = manifest_of(&svc_desc);
    int rc = (int)fw_embed_start(&m);

    if (rc != (int)CM_OK) {
        return rc;
    }
    /* `init()` 跑在 App 自己的任务里，必须先等它把服务登记好。 */
    for (int i = 0; i < 100; i++) {
        if (fw_svc_ready(g_svc_id) == CM_OK) {
            return (int)CM_OK;
        }
        pf_platform_ops()->sleep_ms(5u);
    }
    return (int)fw_svc_ready(g_svc_id);
}

TT_CASE(service_registry_answers_ready_then_unavailable_after_exit)
{
    struct fw_app_ctx *c;

    reset_all();
    g_ev_n = 0;
    g_done_calls = 0;
    g_done_superseded = 0;
    TT_ASSERT_INT((int)CM_OK, start_service_host());
    TT_ASSERT_TRUE(fw_svc_owner(g_svc_id) != NULL);

    c = fw_live_at(0u);
    TT_ASSERT_TRUE(c != NULL);
    fw_app_request_cancel(c);
    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, wait_terminal(c, 2000u));

    /* 崩了/停了不是"这块板没有这个服务"：表项还在、只是没人认领（D23）。 */
    TT_ASSERT_INT((int)CM_ERR_UNAVAILABLE, (int)fw_svc_ready(g_svc_id));
    TT_ASSERT_TRUE(fw_svc_owner(g_svc_id) == NULL);
    reset_all();
}

TT_CASE(submit_copies_the_caller_buffer)
{
    uint8_t src[200];
    uint8_t got[200];
    uint32_t key = 0u;
    size_t len = 0u;

    reset_all();
    TT_ASSERT_INT((int)CM_OK, start_service_host());

    for (size_t i = 0; i < sizeof src; i++) {
        src[i] = 0xAAu;
    }
    TT_ASSERT_INT((int)CM_OK, (int)fw_svc_submit(g_svc_id, 7u, src, sizeof src, on_done, NULL));

    /* 提交返回后立刻改写调用方缓冲：拷贝语义要求这不会影响已提交的帧。 */
    for (size_t i = 0; i < sizeof src; i++) {
        src[i] = 0x5Au;
    }

    TT_ASSERT_INT((int)CM_OK, (int)fw_svc_submit_take(g_svc_id, &key, got, sizeof got, &len));
    TT_ASSERT_INT(7, (int)key);
    TT_ASSERT_INT(200, (int)len);
    TT_ASSERT_INT(0xAA, got[0]);
    TT_ASSERT_INT(0xAA, got[199]);
    TT_ASSERT_INT(1, obs_get(&g_done_calls));
    TT_ASSERT_INT(0, __atomic_load_n(&g_done_superseded, __ATOMIC_ACQUIRE));
    reset_all();
}

TT_CASE(same_key_overwrites_and_notifies_the_old_frame)
{
    uint8_t a[4] = {1u, 1u, 1u, 1u};
    uint8_t b[4] = {2u, 2u, 2u, 2u};
    uint8_t got[8];
    uint32_t key = 0u;
    size_t len = 0u;

    reset_all();
    TT_ASSERT_INT((int)CM_OK, start_service_host());

    TT_ASSERT_INT((int)CM_OK, (int)fw_svc_submit(g_svc_id, 3u, a, sizeof a, on_done, NULL));
    TT_ASSERT_INT((int)CM_OK, (int)fw_svc_submit(g_svc_id, 3u, b, sizeof b, on_done, NULL));
    TT_ASSERT_INT(1, obs_get(&g_done_calls));
    TT_ASSERT_INT(1, __atomic_load_n(&g_done_superseded, __ATOMIC_ACQUIRE));
    TT_ASSERT_INT(1, (int)fw_svc_submit_count(g_svc_id));

    TT_ASSERT_INT((int)CM_OK, (int)fw_svc_submit_take(g_svc_id, &key, got, sizeof got, &len));
    TT_ASSERT_INT(3, (int)key);
    TT_ASSERT_INT(4, (int)len);
    TT_ASSERT_INT(2, got[0]);
    TT_ASSERT_INT(2, got[3]);
    reset_all();
}

TT_CASE(submit_table_full_is_busy_not_nomem)
{
    uint8_t one[2] = {9u, 9u};
    uint8_t got[8];
    uint32_t key = 0u;
    size_t len = 0u;

    reset_all();
    TT_ASSERT_INT((int)CM_OK, start_service_host());

    for (uint32_t k = 1u; k <= FW_SUBMIT_SLOTS; k++) {
        TT_ASSERT_INT((int)CM_OK, (int)fw_svc_submit(g_svc_id, k, one, sizeof one, on_done, NULL));
    }
    TT_ASSERT_INT((int)CM_ERR_BUSY,
                  (int)fw_svc_submit(g_svc_id, 99u, one, sizeof one, on_done, NULL));
    TT_ASSERT_INT((int)FW_SUBMIT_SLOTS, (int)fw_svc_submit_count(g_svc_id));

    for (uint32_t k = 1u; k <= FW_SUBMIT_SLOTS; k++) {
        TT_ASSERT_INT((int)CM_OK, (int)fw_svc_submit_take(g_svc_id, &key, got, sizeof got, &len));
    }
    TT_ASSERT_INT(0, (int)fw_svc_submit_count(g_svc_id));
    TT_ASSERT_INT((int)CM_ERR_NOT_SUPPORTED,
                  (int)fw_svc_submit_take(g_svc_id, &key, got, sizeof got, &len));
    reset_all();
}

TT_CASE(busy_loop_is_detected_but_cancel_does_not_stop_it)
{
    fw_manifest_t m = manifest_of(&spin_desc);
    struct fw_app_ctx *c;

    reset_all();
    __atomic_store_n(&g_spin, 1, __ATOMIC_RELEASE);

    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    c = fw_live_at(0u);
    TT_ASSERT_TRUE(c != NULL);

    /* 150 ms 一次心跳都没喂：必须被发现。 */
    pf_platform_ops()->sleep_ms(150u);
    TT_ASSERT_INT(1, fw_app_is_stalled(&c->pub, 100u));

    /* 置取消位也救不了它——它压根不经过任何阻塞接口。 */
    fw_app_request_cancel(c);
    pf_platform_ops()->sleep_ms(200u);
    TT_ASSERT_INT((int)FW_APP_STATE_RUNNING, (int)fw_app_state(c));
    TT_ASSERT_INT(1, fw_app_is_stalled(&c->pub, 100u));

    /* 测试自己把它放掉，免得固件带着一个转圈的任务过日子；这一下不是"取消"
     * 生效，而是任务从 `task()` 返回后走 FAILED 路径。 */
    __atomic_store_n(&g_spin, 0, __ATOMIC_RELEASE);
    TT_ASSERT_INT((int)FW_APP_STATE_FAILED, wait_terminal(c, 1000u));
    TT_ASSERT_INT(1, obs_get(&g_exit_calls));
    reset_all();
}

/* 固件里只允许有一个 `main`（本工程是 ESP-IDF 的 `app_main` 入口，`main` 这个
 * 符号留给套件）。两个套件都要各自的入口名，所以按"是不是在 IDF 里"选择：
 * 设备侧叫 `app_behavior_main`，host 侧仍是 `main`。 */
/* ------------------------------------------------------------------ */
/* 串口能力（第一项真实能力的真机证据）                                  */
/* ------------------------------------------------------------------ */

/* 板上能验的只有三件事，别的都是自欺：
 *   1. 板子真的把实现交上来了（`cap_available(CAP_UART)` 为真）；
 *   2. 写出去不崩、字节数如实返回；
 *   3. **没有对端时读会如实超时**，而不是假装读到 0 字节成功。
 * "对端收到了吗"要另一头的人看串口助手——那一条不属于本套件。
 *
 * 所有串口操作都发生在 App 的 `init()` 里：`uart_write()`/`uart_read()` 要求
 * App 上下文（伪阻塞要靠取消位），而 `uart_open()` 必须在板级钩子跑过之后——
 * 那个钩子在 `fw_embed_start()` 里，所以顺序只能是"先启动、再在 App 里操作"。 */
static int g_uart_scenario;
static cm_err_t g_uart_rc; /* 写/读的结果 */
static cm_err_t g_uart_open_rc;
static cm_err_t g_uart_close_rc;
static cm_err_t g_uart_double_rc;
static size_t g_uart_n;
static char g_uart_buf[16];
static int g_uart_done;

static cm_err_t uart_init(app_ctx_t *ctx)
{
    (void)ctx;
    g_uart_rc = CM_OK;
    g_uart_open_rc = CM_OK;
    g_uart_close_rc = CM_OK;
    g_uart_double_rc = CM_OK;
    g_uart_n = 0u;

    switch (g_uart_scenario) {
    case 1: /* 开 → 写 4 字节 → 关 */
        g_uart_open_rc = uart_open(UART_PORT_APP, 115200u);
        if (g_uart_open_rc == CM_OK) {
            g_uart_rc = uart_write(UART_PORT_APP, "ping", 4u, &g_uart_n);
            g_uart_close_rc = uart_close(UART_PORT_APP);
        }
        break;
    case 2: /* 开 → 没接线时读 50ms → 关 */
        g_uart_open_rc = uart_open(UART_PORT_APP, 115200u);
        if (g_uart_open_rc == CM_OK) {
            g_uart_rc = uart_read(UART_PORT_APP, g_uart_buf, sizeof g_uart_buf, &g_uart_n, 50u);
            g_uart_close_rc = uart_close(UART_PORT_APP);
        }
        break;
    case 3: /* 重复打开必须 BUSY，两次关闭第二次必须 STATE */
        g_uart_open_rc = uart_open(UART_PORT_APP, 115200u);
        g_uart_double_rc = uart_open(UART_PORT_APP, 115200u);
        g_uart_rc = uart_close(UART_PORT_APP);
        g_uart_close_rc = uart_close(UART_PORT_APP);
        break;
    default:
        break;
    }
    __atomic_store_n(&g_uart_done, 1, __ATOMIC_RELEASE);
    return CM_OK;
}

static const app_desc_t uart_desc = {
    .id = 21u,
    .name = "uart-probe",
    .init = uart_init,
    .exit = ok_exit,
    .task = ok_task,
    .flags = APP_FLAG_SYSTEM, /* 清单里的 App 要由 fw_boot() 拉起来，得带这个标志 */
    .stack_bytes = APP_STACK_MIN,
};

static void uart_case_reset(void)
{
    reset_all();
    g_uart_scenario = 0;
    __atomic_store_n(&g_uart_done, 0, __ATOMIC_RELEASE);
}

TT_CASE(uart_is_published_by_the_board)
{
    fw_manifest_t m;

    uart_case_reset();
    m = manifest_of(&uart_desc);
    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    TT_ASSERT_TRUE(cap_available(CAP_UART));
    TT_ASSERT_INT(1u, uart_port_count());
    uart_case_reset();
}

TT_CASE(uart_open_write_close_round_trip)
{
    fw_manifest_t m;

    uart_case_reset();
    m = manifest_of(&uart_desc);
    g_uart_scenario = 1;
    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    TT_ASSERT_TRUE(wait_until_truthy(&g_uart_done, 1, 2000u));
    TT_ASSERT_INT(CM_OK, g_uart_open_rc);
    TT_ASSERT_INT(CM_OK, g_uart_rc);
    TT_ASSERT_INT(4, (int)g_uart_n);
    TT_ASSERT_INT(CM_OK, g_uart_close_rc);
    uart_case_reset();
}

TT_CASE(uart_read_times_out_with_nothing_wired)
{
    fw_manifest_t m;

    uart_case_reset();
    m = manifest_of(&uart_desc);
    g_uart_scenario = 2;
    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    TT_ASSERT_TRUE(wait_until_truthy(&g_uart_done, 1, 3000u));
    TT_ASSERT_INT(CM_OK, g_uart_open_rc);
    TT_ASSERT_INT(CM_ERR_TIMEOUT, g_uart_rc); /* 没接对端：如实说"没等到"，不是 0 字节成功 */
    TT_ASSERT_INT(0, (int)g_uart_n);
    TT_ASSERT_INT(CM_OK, g_uart_close_rc);
    uart_case_reset();
}

TT_CASE(uart_double_open_is_busy_and_close_is_paired)
{
    fw_manifest_t m;

    uart_case_reset();
    m = manifest_of(&uart_desc);
    g_uart_scenario = 3;
    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    TT_ASSERT_TRUE(wait_until_truthy(&g_uart_done, 1, 2000u));
    TT_ASSERT_INT(CM_OK, g_uart_open_rc);
    TT_ASSERT_INT(CM_ERR_BUSY, g_uart_double_rc);
    TT_ASSERT_INT(CM_OK, g_uart_rc);              /* 第一次关闭成功 */
    TT_ASSERT_INT(CM_ERR_STATE, g_uart_close_rc); /* 第二次没有可关的 */
    uart_case_reset();
}

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
    TT_RUN(service_registry_answers_ready_then_unavailable_after_exit);
    TT_RUN(submit_copies_the_caller_buffer);
    TT_RUN(same_key_overwrites_and_notifies_the_old_frame);
    TT_RUN(submit_table_full_is_busy_not_nomem);
    TT_RUN(busy_loop_is_detected_but_cancel_does_not_stop_it);
    TT_RUN(uart_is_published_by_the_board);
    TT_RUN(uart_open_write_close_round_trip);
    TT_RUN(uart_read_times_out_with_nothing_wired);
    TT_RUN(uart_double_open_is_busy_and_close_is_paired);
    return tt_summary();
}
