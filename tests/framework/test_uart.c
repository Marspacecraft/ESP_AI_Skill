/* tests/framework/test_uart.c —— 串口能力的第一层证据（host）。
 *
 * 验的是框架这一层：能力判定、参数校验、切片与超时、错误码翻译、以及"记账"。
 * 不验硬件——假串口在 `platform/host-fake/`，真机证据在 `tests/port/`。
 *
 * 一条设计约束在用例里被钉死：`uart_write()`/`uart_read()` **必须有 App 上下文**。
 * 它们靠取消位实现伪阻塞，从 main 里调就没人能取消它——所以那是 `CM_ERR_STATE`
 * 而不是"先这样，以后再说"。
 */
#include "api/af_api/app.h"
#include "api/af_api/cap.h"
#include "api/af_api/mgr.h"
#include "api/af_api/uart.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"
#include "common/include/cm_atomic.h"
#include "tests/tinytest.h"

#define PROBE_ID 7u

/* 被测 App 的"剧本"：在 init() 里做一次调用，把结果留给主任务断言。 */
static int g_scenario;
static cm_err_t g_rc;
static size_t g_n;
static unsigned char g_buf[16];
static uint32_t g_done; /* 跨任务标志：只能走 CM_ATOMIC_*（D51） */

static cm_err_t probe_init(app_ctx_t *ctx)
{
    (void)ctx;
    g_rc = CM_OK;
    g_n = 0u;

    switch (g_scenario) {
    case 1: { /* 写 5 字节 */
        static const char msg[] = "hello";
        g_rc = uart_write(UART_PORT_APP, msg, sizeof msg - 1u, &g_n);
        break;
    }
    case 2: { /* 读：静默 20ms */
        g_rc = uart_read(UART_PORT_APP, g_buf, sizeof g_buf, &g_n, 20u);
        break;
    }
    case 3: { /* 读：有 3 字节 */
        g_rc = uart_read(UART_PORT_APP, g_buf, sizeof g_buf, &g_n, 20u);
        break;
    }
    case 4: { /* 关掉之后再写 */
        (void)uart_close(UART_PORT_APP);
        g_rc = uart_write(UART_PORT_APP, "x", 1u, &g_n);
        break;
    }
    default:
        break;
    }
    CM_ATOMIC_STORE(&g_done, 1u, CM_MEM_RELEASE);
    return CM_OK;
}

static void probe_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static void probe_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        fw_delay_block(50u);
    }
}

static const app_desc_t s_probe = {
    .id = PROBE_ID,
    .name = "uart-probe",
    .init = probe_init,
    .exit = probe_exit,
    .task = probe_task,
    .flags = 0u,
    .stack_bytes = APP_STACK_MIN,
};

/* 清单里那一个（id 7）与真正跑剧本的实例（id 9）分开：清单里的 id 已经被注册表占住，
 * 再拿同一个 id 建实例会被 `fw_app_start()` 拒掉——这不是测试技巧，是 D18 的规矩。 */
static const app_desc_t s_worker = {
    .id = 9u,
    .name = "uart-worker",
    .init = probe_init,
    .exit = probe_exit,
    .task = probe_task,
    .flags = 0u,
    .stack_bytes = APP_STACK_MIN,
};

static const app_desc_t *const s_apps[] = {&s_probe};
static const fw_manifest_t s_man = {s_apps, 1u};

static void reset_all(void)
{
    hf_uart_reset();
    /* 表里每释放一个就少一个，所以"每次取第 0 个"而不是按下标遍历——
     * 边遍历边删会让下标跳过元素（旧仓库踩过：漏停一个任务，后面的用例活在脏世界里）。 */
    while (fw_live_count() > 0u) {
        fw_app_ctx_t *c = fw_live_at(0u);
        int guard = 0;

        fw_app_request_cancel(c);
        while (fw_app_state(c) == FW_APP_STATE_RUNNING || fw_app_state(c) == FW_APP_STATE_INIT) {
            if (++guard > 2000) {
                break;
            }
            pf_host_fake_ops.sleep_ms(1u);
        }
        fw_app_ctx_free(c);
    }
    fw_reset();
    CM_ATOMIC_STORE(&g_done, 0u, CM_MEM_RELEASE);
    g_scenario = 0;
}

static void boot(bool with_board_hook)
{
    reset_all();
    if (with_board_hook) {
        (void)fw_startup(pf_host_fake_ops_get(), &s_man);
    } else {
        /* 只绑表、不跑板级钩子：模拟"硬件在，但软件没交实现" */
        (void)fw_platform_bind(pf_host_fake_ops_get());
        (void)fw_init(&s_man, 1u);
    }
}

static void run_probe(int scenario)
{
    fw_app_ctx_t *c;

    g_scenario = scenario;
    CM_ATOMIC_STORE(&g_done, 0u, CM_MEM_RELEASE);
    c = fw_app_ctx_new_with_id(&s_worker, 9u);
    TT_ASSERT_TRUE(c != NULL);
    if (c == NULL) {
        return;
    }
    TT_ASSERT_INT(CM_OK, fw_app_start(c));
}

static void wait_done(void)
{
    int guard = 0;

    while (CM_ATOMIC_LOAD(&g_done, CM_MEM_ACQUIRE) == 0u) {
        if (++guard > 2000) {
            break;
        }
        pf_host_fake_ops.sleep_ms(1u);
    }
}

TT_CASE(hardware_reported_without_implementation_is_not_supported)
{
    boot(false);
    TT_ASSERT_INT(0u, uart_port_count());
    TT_ASSERT_FALSE(cap_available(CAP_UART)); /* 板子报了位，但没人交出实现 */
    TT_ASSERT_INT(CM_ERR_NOT_SUPPORTED, uart_open(UART_PORT_APP, 115200u));
}

TT_CASE(board_hook_publishes_one_port)
{
    boot(true);
    TT_ASSERT_INT(1u, uart_port_count());
    TT_ASSERT_TRUE(cap_available(CAP_UART));
}

TT_CASE(open_validates_arguments_and_refuses_double_open)
{
    boot(true);
    TT_ASSERT_INT(CM_ERR_INVALID, uart_open(1u, 115200u)); /* 只有一路 */
    TT_ASSERT_INT(CM_ERR_INVALID, uart_open(UART_PORT_APP, 0u));
    TT_ASSERT_INT(CM_OK, uart_open(UART_PORT_APP, 115200u));
    TT_ASSERT_INT(CM_ERR_BUSY, uart_open(UART_PORT_APP, 115200u));
    TT_ASSERT_INT(CM_OK, uart_close(UART_PORT_APP));
    TT_ASSERT_INT(CM_ERR_STATE, uart_close(UART_PORT_APP)); /* 没打开就没有可关的 */
}

TT_CASE(write_moves_every_byte_and_reports_count)
{
    char got[16] = {0};
    size_t n;

    boot(true);
    TT_ASSERT_INT(CM_OK, uart_open(UART_PORT_APP, 115200u));
    run_probe(1);
    wait_done();
    TT_ASSERT_INT(CM_OK, g_rc);
    TT_ASSERT_INT(5, (int)g_n);
    n = hf_uart_take(got, sizeof got);
    TT_ASSERT_INT(5, (int)n);
    TT_ASSERT_STR("hello", got);
}

TT_CASE(read_times_out_when_quiet_and_returns_data_when_fed)
{
    boot(true);
    TT_ASSERT_INT(CM_OK, uart_open(UART_PORT_APP, 115200u));

    run_probe(2); /* 没有数据：等满 20ms 应报超时，而不是骗人说 0 字节成功 */
    wait_done();
    TT_ASSERT_INT(CM_ERR_TIMEOUT, g_rc);
    TT_ASSERT_INT(0, (int)g_n);

    hf_uart_feed("abc", 3u);
    run_probe(3);
    wait_done();
    TT_ASSERT_INT(CM_OK, g_rc);
    TT_ASSERT_INT(3, (int)g_n);
    TT_ASSERT_INT('a', (int)g_buf[0]);
    TT_ASSERT_INT('c', (int)g_buf[2]);
}

TT_CASE(blocking_calls_require_an_app_context)
{
    boot(true);
    TT_ASSERT_INT(CM_OK, uart_open(UART_PORT_APP, 115200u));
    /* 主任务里没有取消位可查 → 拒绝，而不是"悄悄阻塞" */
    TT_ASSERT_INT(CM_ERR_STATE, uart_write(UART_PORT_APP, "x", 1u, NULL));
    TT_ASSERT_INT(CM_ERR_STATE, uart_read(UART_PORT_APP, g_buf, sizeof g_buf, &g_n, 0u));
}

TT_CASE(write_after_close_is_state_error)
{
    boot(true);
    TT_ASSERT_INT(CM_OK, uart_open(UART_PORT_APP, 115200u));
    run_probe(4);
    wait_done();
    TT_ASSERT_INT(CM_ERR_STATE, g_rc);
    TT_ASSERT_INT(0, (int)g_n);
}

int main(void)
{
    tt_set_program("test_uart");
    TT_RUN(hardware_reported_without_implementation_is_not_supported);
    TT_RUN(board_hook_publishes_one_port);
    TT_RUN(open_validates_arguments_and_refuses_double_open);
    TT_RUN(write_moves_every_byte_and_reports_count);
    TT_RUN(read_times_out_when_quiet_and_returns_data_when_fed);
    TT_RUN(blocking_calls_require_an_app_context);
    TT_RUN(write_after_close_is_state_error);
    return tt_summary();
}
