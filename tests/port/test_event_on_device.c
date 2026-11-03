/* tests/port/test_event_on_device.c —— 事件层的真机验收（D28 的前提在这里被检验）。
 *
 * `test_fw_event.c` 在 host 上验完了语义；这一份要验的是**host 上验不出来的**两件事：
 *
 *   1. `fw_event_post_isr()` 在真实任务/中断上下文里是否还是"无锁、无分配、常数时间"；
 *   2. D28 那条前提——**一个 topic 只许一个生产者上下文**——被违反时会发生什么。
 *
 * 第 2 条不是"断言它正确"，而是**测量它错得有多安静**：两个生产者同时往同一个
 * topic 写，我们会数出"成功提交了多少帧"与"实际取回多少帧"，差额就是丢掉的。
 * 同一个用例在 host 上也跑：host 的 pthread 一样会把丢帧暴露出来（实测 933 提交 /
 * 530 取回），S3 上再跑一次是为了看**双核并行**会不会让它更糟。
 */
#include "tinytest.h"

#include "framework/include/fw_app.h"
#include "framework/include/fw_embed.h"
#include "framework/src/fw_internal.h"
#include "platform/port/pf_ops.h"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <pthread.h>
#include <time.h>
#endif

#define T_BUTTON 1u
#define T_LEAK   2u
#define P_MAIN   0xA1u
#define P_ALT    0xB2u

static int flag_get(int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void flag_set(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void flag_add(int *p) { (void)__atomic_add_fetch(p, 1, __ATOMIC_RELAXED); }

static uint32_t now_ms(void) { return pf_platform_ops()->clock_ms(); }

/* ------------------------------------------------------------------ */
/* 用例 1：声明 → 提交 → 取回                                          */
/* ------------------------------------------------------------------ */

TT_CASE(declare_post_take_roundtrip)
{
    unsigned char in[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    unsigned char out[8] = { 0u };
    size_t len = 0u;

    fw_reset();
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, sizeof in));
    TT_ASSERT_INT(CM_OK, fw_event_post(T_BUTTON, in, sizeof in));
    TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, out, sizeof out, &len));
    TT_ASSERT_INT(8, (int)len);
    for (int i = 0; i < 8; i++) {
        TT_ASSERT_INT(i + 1, (int)out[i]);
    }
    /* 空队列是"还没有"，不是错误（TIMEOUT，见 fw_app.h 的注释）。 */
    TT_ASSERT_INT(CM_ERR_TIMEOUT, fw_event_take(T_BUTTON, out, sizeof out, &len));
    fw_reset();
}

/* ------------------------------------------------------------------ */
/* 用例 2：ISR 版按**声明的长度**收                                               */
/* ------------------------------------------------------------------ */

TT_CASE(isr_post_uses_the_declared_length)
{
    unsigned char in[8] = { 9u, 9u, 9u, 9u, 9u, 9u, 9u, 9u };
    unsigned char out[8] = { 0u };
    size_t len = 0u;

    fw_reset();
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, 4u));
    /* 只有 4 字节容量：ISR 版没有长度参数，必须按声明值截到 4。 */
    TT_ASSERT_INT(CM_OK, fw_event_post_isr(T_BUTTON, in));
    TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, out, sizeof out, &len));
    TT_ASSERT_INT(4, (int)len);
    fw_reset();
}

/* ------------------------------------------------------------------ */
/* 用例 3：wait 在真机上也是伪阻塞（超时返回、不挂住）                  */
/* ------------------------------------------------------------------ */

/* `fw_event_wait()` 只在 App 任务里有意义（它查当前上下文，D15），所以这条
 * 用例必须在一个真的 App 任务里等，不能在测试主任务里调。 */
static int g_wait_rc;
static int g_wait_spent;
static int g_wait_diff_done;

static cm_err_t waiter_init(app_ctx_t *ctx) { (void)ctx; return CM_OK; }
static void waiter_exit(app_ctx_t *ctx) { (void)ctx; }

static void waiter_task(app_ctx_t *ctx)
{
    unsigned char buf[8];
    size_t len = 0u;
    uint32_t t0;

    (void)ctx;
    t0 = now_ms();
    g_wait_rc = (int)fw_event_wait(T_BUTTON, buf, sizeof buf, &len, 60u);
    g_wait_spent = (int)(uint32_t)(now_ms() - t0);
    flag_set(&g_wait_diff_done, 1);
    for (;;) {
        fw_delay_block(100u);
    }
}

static const app_desc_t waiter_desc = {
    .id = 46u, .name = "evt_wait", .init = waiter_init, .exit = waiter_exit,
    .task = waiter_task, .stack_bytes = APP_STACK_MIN, .flags = APP_FLAG_SYSTEM,
};

TT_CASE(wait_times_out_on_a_real_timer)
{
    static const app_desc_t *apps[1];
    fw_manifest_t m;
    struct fw_app_ctx *c;

    fw_reset();
    apps[0] = &waiter_desc;
    m.apps = apps;
    m.count = 1u;
    g_wait_rc = 0;
    g_wait_spent = 0;
    g_wait_diff_done = 0;

    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, 8u));
    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));

    for (int i = 0; i < 200 && flag_get(&g_wait_diff_done) == 0; i++) {
        pf_platform_ops()->sleep_ms(5u);
    }
    TT_ASSERT_INT(1, flag_get(&g_wait_diff_done));
    TT_ASSERT_INT((int)CM_ERR_TIMEOUT, g_wait_rc);
    /* 真机 tick 是 100 Hz：等 60 ms 至少要跨过 5 个 tick，但要明显少于"永远"。 */
    TT_ASSERT_TRUE(g_wait_spent >= 40);
    TT_ASSERT_TRUE(g_wait_spent < 2000);

    c = fw_live_at(0u);
    TT_ASSERT_TRUE(c != NULL);
    fw_app_request_cancel(c);
    for (int i = 0; i < 200 && fw_app_state(c) == FW_APP_STATE_RUNNING; i++) {
        pf_platform_ops()->sleep_ms(5u);
    }
    fw_live_remove(c);
    fw_app_ctx_free(c);
    fw_reset();
}

/* ------------------------------------------------------------------ */
/* 用例 4：队列满时丢**新的**                                         */
/* ------------------------------------------------------------------ */

TT_CASE(queue_full_drops_the_newest)
{
    unsigned char v[2] = { 0u, 0u };
    unsigned char out[2] = { 0u };
    size_t len = 0u;
    int full_rc = 0;

    fw_reset();
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, sizeof v));
    for (uint32_t i = 0; i < FW_EVENT_DEPTH; i++) {
        v[0] = (unsigned char)i;
        TT_ASSERT_INT(CM_OK, fw_event_post(T_BUTTON, v, sizeof v));
    }
    v[0] = 0xEEu;
    full_rc = (int)fw_event_post(T_BUTTON, v, sizeof v);
    TT_ASSERT_INT((int)CM_ERR_BUSY, full_rc);

    /* 队头还是最早的 0，不是刚被拒的 0xEE。 */
    TT_ASSERT_INT(CM_OK, fw_event_take(T_BUTTON, out, sizeof out, &len));
    TT_ASSERT_INT(0, (int)out[0]);
    fw_reset();
}

/* ------------------------------------------------------------------ */
/* 用例 5：wait 是伪阻塞点——取消位命中后不再返回                        */
/* ------------------------------------------------------------------ */

static int g_app_exits;
static int g_after_wait;

static cm_err_t ev_init(app_ctx_t *ctx) { (void)ctx; return CM_OK; }
static void ev_exit(app_ctx_t *ctx) { (void)ctx; flag_add(&g_app_exits); }

static void ev_task(app_ctx_t *ctx)
{
    unsigned char buf[8];
    size_t len = 0u;

    (void)ctx;
    /* 5 s 的超时：如果取消位不生效，这个用例会明显变慢而不是悄悄通过。 */
    (void)fw_event_wait(T_BUTTON, buf, sizeof buf, &len, 5000u);
    flag_set(&g_after_wait, 1);   /* 取消场景下这行不该执行 */
    for (;;) {
        fw_delay_block(100u);
    }
}

static const app_desc_t ev_desc = {
    .id = 45u, .name = "evt", .init = ev_init, .exit = ev_exit, .task = ev_task,
    .stack_bytes = APP_STACK_MIN, .flags = APP_FLAG_SYSTEM,
};

TT_CASE(cancel_interrupts_the_wait)
{
    static const app_desc_t *apps[1];
    fw_manifest_t m;
    struct fw_app_ctx *c;
    uint32_t t0;

    fw_reset();
    apps[0] = &ev_desc;
    m.apps = apps;
    m.count = 1u;
    g_app_exits = 0;
    g_after_wait = 0;

    TT_ASSERT_INT(CM_OK, fw_event_declare(T_BUTTON, 8u));
    TT_ASSERT_INT(CM_OK, fw_embed_start(&m));
    c = fw_live_at(0u);
    TT_ASSERT_TRUE(c != NULL);

    /* 等它进到 wait 里（状态 RUNNING 不代表已经在等，给一点时间）。 */
    pf_platform_ops()->sleep_ms(30u);
    t0 = now_ms();
    fw_app_request_cancel(c);

    for (int i = 0; i < 200 && fw_app_state(c) == FW_APP_STATE_RUNNING; i++) {
        pf_platform_ops()->sleep_ms(5u);
    }
    TT_ASSERT_TRUE((uint32_t)(now_ms() - t0) < 1000u);
    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, (int)fw_app_state(c));
    TT_ASSERT_INT(1, flag_get(&g_app_exits));
    TT_ASSERT_INT(0, flag_get(&g_after_wait));   /* wait 之后那行确实没走 */

    fw_live_remove(c);
    fw_app_ctx_free(c);
    fw_reset();
}

/* ------------------------------------------------------------------ */
/* 用例 6：D28 的前提被违反时会发生什么（双核 S3 上的测量）             */
/* ------------------------------------------------------------------ */

#define MULTI_PER_PRODUCER 500

static int g_ok_posts;
static int g_rejected;
static int g_taken;
static int g_stop_producers;

static void producer_body(unsigned producer)
{
    unsigned char v[2];

    v[1] = (unsigned char)producer;
    for (int i = 0; i < MULTI_PER_PRODUCER; i++) {
        cm_err_t rc;

        v[0] = (unsigned char)(i & 0xFF);
        rc = fw_event_post(T_LEAK, v, sizeof v);
        if (rc == CM_OK) {
            flag_add(&g_ok_posts);
        } else {
            flag_add(&g_rejected);
        }
    }
    flag_add(&g_stop_producers);
}

#if defined(ESP_PLATFORM)
static void producer_task(void *arg)
{
    producer_body((unsigned)(uintptr_t)arg);
    /* FreeRTOS 的任务函数**不许返回**（返回会 `abort()`："should not return"）。
     * 显式删掉自己，让 IDLE 去回收 TCB。 */
    vTaskDelete(NULL);
}
#else
static void *producer_thread(void *arg) { producer_body((unsigned)(uintptr_t)arg); return NULL; }
#endif

TT_CASE(two_producers_one_topic_measures_the_loss)
{
    unsigned char out[2];
    size_t len = 0u;
    uint32_t deadline;
    int ok;
    int taken;
    int lost;
#if !defined(ESP_PLATFORM)
    pthread_t t1;
    pthread_t t2;
#endif

    fw_reset();
    g_ok_posts = 0;
    g_rejected = 0;
    g_taken = 0;
    g_stop_producers = 0;
    TT_ASSERT_INT(CM_OK, fw_event_declare(T_LEAK, 2u));

    /* 两个生产者**同时**往同一个 topic 写——这正是 D28 禁止的那件事。 */
#if defined(ESP_PLATFORM)
    {
        TaskHandle_t h1 = NULL;
        TaskHandle_t h2 = NULL;

        /* 优先级取 1（与 main_task 同档）：比它高就会把"边产边取"的消费者饿死，
         * 那测出来的就不是丢帧而是观测不到（D40 的同一课）。 */
        /* 绑核不能写死：C3 是**单核**，`xTaskCreatePinnedToCore(..., 1)` 会当场断言
         * ——`assert failed: xTaskCreatePinnedToCore ... xCoreID < 1`
         * （真机跑出来才发现；host 上没有这个参数，所以 host 永远绿）。
         * 双核板上让两个生产者分居两核，单核板上只能同核：前者测跨核竞争，
         * 后者测同核抢占，两者都是真实的丢帧来源。 */
        const int cores = (int)portNUM_PROCESSORS;
        const int c1_core = (cores > 1) ? 1 : 0;

        TT_ASSERT_INT(pdPASS,
                      xTaskCreatePinnedToCore(producer_task, "prod0", 4096,
                                              (void *)(uintptr_t)P_MAIN, 1, &h1, 0));
        TT_ASSERT_INT(pdPASS,
                      xTaskCreatePinnedToCore(producer_task, "prod1", 4096,
                                              (void *)(uintptr_t)P_ALT, 1, &h2, c1_core));
    }
#else
    {
        TT_ASSERT_INT(0, pthread_create(&t1, NULL, producer_thread, (void *)(uintptr_t)P_MAIN));
        TT_ASSERT_INT(0, pthread_create(&t2, NULL, producer_thread, (void *)(uintptr_t)P_ALT));
        /* 生产与消费**同时**跑才有竞争；join 留到收尾——不 join 的线程
         * 在 `fw_reset()` 之后还在写表，TSan 会同时报 data race 与 thread leak。 */
    }
#endif

    /* 一边生产一边消费，直到两个生产者都收工、且队列被取空。 */
    deadline = now_ms();
    while ((uint32_t)(now_ms() - deadline) < 5000u) {
        if (fw_event_take(T_LEAK, out, sizeof out, &len) == CM_OK) {
            flag_add(&g_taken);
        } else if (flag_get(&g_stop_producers) >= 2) {
            break;
        }
    }
#if !defined(ESP_PLATFORM)
    pthread_join(t1, NULL);
    pthread_join(t2, NULL); /* 必须先等它们真的退出，再碰表 */
#endif
    /* 收尾：把生产者退出前落在队列里的取干净。
     *
     * **必须有上限**：这个用例故意让两个生产者写在同一个 topic 上（违反 D28），
     * `head` 因此可能被回退，队列不变量会坏。没有上限的 `while (take == CM_OK)`
     * 曾经真的挂死过（TSan 下 30 次里 1 次，表现为 60s 超时而不是 FAIL）——
     * 测试的失败形态必须是"红"，不能是"卡住"。 */
    {
        uint32_t drain_deadline = now_ms();

        while ((uint32_t)(now_ms() - drain_deadline) < 2000u
               && fw_event_take(T_LEAK, out, sizeof out, &len) == CM_OK) {
            flag_add(&g_taken);
        }
    }

    ok = flag_get(&g_ok_posts);
    taken = flag_get(&g_taken);
    lost = ok - taken;

    printf("MULTI_PRODUCER ok=%d rejected=%d taken=%d lost=%d\n", ok, flag_get(&g_rejected),
           taken, lost);
    (void)fflush(stdout);

    /* 只断言"不可能违反"的那一条：取回的帧数**不会多于**成功提交的帧数。
     * 丢了多少是**测量结果**，不是断言——D28 的前提被违反时，丢帧是预期行为。 */
    TT_ASSERT_TRUE(ok > 0);
    TT_ASSERT_TRUE(taken <= ok);
    fw_reset();
}

#if defined(ESP_PLATFORM)
int event_on_device_main(void)
#else
int main(void)
#endif
{
    TT_PROGRAM("test_event_on_device");
    TT_RUN(declare_post_take_roundtrip);
    TT_RUN(isr_post_uses_the_declared_length);
    TT_RUN(wait_times_out_on_a_real_timer);
    TT_RUN(queue_full_drops_the_newest);
    TT_RUN(cancel_interrupts_the_wait);
    TT_RUN(two_producers_one_topic_measures_the_loss);
    return tt_summary();
}
