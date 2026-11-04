/* tests/framework/test_res.c —— 槽位式资源（步骤 6.1）。
 *
 * 验的不是"能分配"，而是三件事：**槽位是唯一权威**、**退出路径恰好回收一次**、
 * **失败路径不留半个登记**。中心用例是第二条：App 在 init 里拿三个资源然后被停，
 * 框架必须把它们全收回去——多了等于重复释放（host-fake 会 abort），少了就是泄漏。
 */
#include "tinytest.h"

#include "apps/app_manifest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

static fw_app_ctx_t *g_ctx;
static int g_acquired;      /* init 里成功 acquire 的次数 */
static int g_exit_saw_slot; /* exit() 里 slots[0] 还非空吗 */
static int g_probe_ok;      /* 槽位里的内存真的可读写 */
static int g_busy_seen;     /* 往非空槽位再 acquire 是否 BUSY */
static int g_bad_seen;      /* 参数非法是否 INVALID */
static int g_reuse_ok;      /* release 之后同一个槽位能再 acquire */

static void idle_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        fw_delay_block(20u);
    }
}

/* ── 场景一：三个 APP 生命周期资源 ───────────────────────────── */
static cm_err_t hold_init(app_ctx_t *ctx)
{
    for (int i = 0; i < 3; i++) {
        if (res_acquire(&ctx->slots[i], RES_KIND_MEM, 32u, RES_LIFE_APP) != CM_OK) {
            return CM_ERR_NOMEM;
        }
        g_acquired++;
    }
    /* 槽位里的内存必须真的能写能读，否则"拿到了"只是拿到了一个数字。 */
    if (ctx->slots[0] != NULL) {
        unsigned char *p = (unsigned char *)ctx->slots[0];
        for (int k = 0; k < 32; k++) {
            p[k] = (unsigned char)k;
        }
        g_probe_ok = 1;
        for (int k = 0; k < 32; k++) {
            if (p[k] != (unsigned char)k) {
                g_probe_ok = 0;
            }
        }
    }
    /* 同一个槽位再来一次：不许覆盖，也不许静默取消。 */
    if (res_acquire(&ctx->slots[0], RES_KIND_MEM, 8u, RES_LIFE_APP) == CM_ERR_BUSY) {
        g_busy_seen = 1;
    }
    /* 不是自己的槽位 / 0 字节：都必须是 INVALID。 */
    if (res_acquire(&ctx->id, RES_KIND_MEM, 8u, RES_LIFE_APP) == CM_ERR_INVALID &&
        res_acquire(&ctx->slots[7], RES_KIND_MEM, 0u, RES_LIFE_APP) == CM_ERR_INVALID) {
        g_bad_seen = 1;
    }
    /* 上面两次失败不许留下任何登记。 */
    if (ctx->slots[7] != NULL) {
        g_bad_seen = 0;
    }
    return CM_OK;
}

/* ── 场景三：提前 release，再复用同一个槽位 ──────────────────── */
static cm_err_t reuse_init(app_ctx_t *ctx)
{
    if (res_acquire(&ctx->slots[0], RES_KIND_MEM, 16u, RES_LIFE_APP) != CM_OK) {
        return CM_ERR_NOMEM;
    }
    res_release(&ctx->slots[0]); /* 提前还 */
    if (ctx->slots[0] != NULL) { /* 还完槽位必须立刻变空 */
        return CM_ERR_STATE;
    }
    res_release(&ctx->slots[0]); /* 重复还：无操作，不许炸 */
    if (res_acquire(&ctx->slots[0], RES_KIND_MEM, 24u, RES_LIFE_APP) != CM_OK) {
        return CM_ERR_NOMEM;
    }
    g_reuse_ok = (ctx->slots[0] != NULL) ? 1 : 0;
    return CM_OK;
}

static void reuse_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static void hold_exit(app_ctx_t *ctx)
{
    /* 第 ③ 步在 exit() **之后**：此时槽位必须还看得见。 */
    g_exit_saw_slot = (ctx->slots[0] != NULL) ? 1 : 0;
}

/* ── 场景二：一个 BOOT 生命周期资源（借来的，别替我收）─────────── */
static cm_err_t boot_init(app_ctx_t *ctx)
{
    return res_acquire(&ctx->slots[0], RES_KIND_MEM, 64u, RES_LIFE_BOOT);
}

static void boot_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static const app_desc_t hold_desc = {
    30u, "holder", hold_init, hold_exit, idle_task, 0u, APP_STACK_MIN,
};

static const app_desc_t boot_desc = {
    31u, "borrower", boot_init, boot_exit, idle_task, 0u, APP_STACK_MIN,
};

static const app_desc_t reuse_desc = {
    32u, "reuser", reuse_init, reuse_exit, idle_task, 0u, APP_STACK_MIN,
};

static void stop_all(void)
{
    while (fw_live_count() > 0u) {
        fw_app_ctx_t *c = fw_live_at(0u);

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

static void setup(void)
{
    fw_reset();
    g_ctx = NULL;
    g_acquired = 0;
    g_exit_saw_slot = 0;
    g_probe_ok = 0;
    g_busy_seen = 0;
    g_bad_seen = 0;
    g_reuse_ok = 0;
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
}

static fw_app_ctx_t *start(const app_desc_t *d, uint32_t id)
{
    fw_app_ctx_t *c = fw_app_ctx_new_with_id(d, id);

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

static void wait_for(int *flag, int want)
{
    for (int i = 0; i < 400 && *flag != want; i++) {
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
}

TT_CASE(exit_releases_app_lifetime_slots_exactly_once)
{
    setup();
    g_ctx = start(&hold_desc, 30u);
    TT_ASSERT_NOT_NULL(g_ctx);
    wait_for(&g_acquired, 3);

    TT_ASSERT_INT(3, g_acquired);
    TT_ASSERT_INT(1, g_probe_ok);                        /* 槽位里的内存真的可读写 */
    TT_ASSERT_INT(1, g_busy_seen);                       /* 非空槽位拒绝覆盖 */
    TT_ASSERT_INT(1, g_bad_seen);                        /* 坏参数 INVALID 且不留登记 */
    TT_ASSERT_INT(0, (int)fw_app_slots_released(g_ctx)); /* 还在跑：一个都没回收 */

    fw_app_request_cancel(g_ctx);
    for (int i = 0; i < 400; i++) {
        if (fw_app_state(g_ctx) == FW_APP_STATE_STOPPED) {
            break;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, (int)fw_app_state(g_ctx));
    TT_ASSERT_INT(1, g_exit_saw_slot);                   /* exit() 里槽位还在 → 回收发生在其后 */
    TT_ASSERT_INT(3, (int)fw_app_slots_released(g_ctx)); /* 恰好三个 */
    for (uint32_t i = 0; i < APP_CTX_SLOTS; i++) {
        TT_ASSERT_TRUE(g_ctx->pub.slots[i] == NULL);
    }
    stop_all();
}

TT_CASE(boot_lifetime_slot_survives_exit)
{
    const pf_ops_t *ops = pf_host_fake_ops_get();

    setup();
    g_ctx = start(&boot_desc, 31u);
    TT_ASSERT_NOT_NULL(g_ctx);

    fw_app_request_cancel(g_ctx);
    for (int i = 0; i < 400; i++) {
        if (fw_app_state(g_ctx) == FW_APP_STATE_STOPPED) {
            break;
        }
        ops->sleep_ms(2u);
    }
    TT_ASSERT_INT((int)FW_APP_STATE_STOPPED, (int)fw_app_state(g_ctx));
    /* 借来的东西：框架不许替他收。回收计数也不该记它。 */
    TT_ASSERT_INT(0, (int)fw_app_slots_released(g_ctx));
    TT_ASSERT_TRUE(g_ctx->pub.slots[0] != NULL);
    /* 借用者要自己交还；这里模拟"下一个 Owner"接手后释放，避免测试漏内存。
     * 注意用 `fw_free`：槽位里的指针是 `fw_alloc` 发的，带 24/16 字节头。 */
    fw_free(g_ctx->pub.slots[0]);
    g_ctx->pub.slots[0] = NULL;
    stop_all();
}

TT_CASE(acquire_outside_an_app_task_is_state)
{
    setup();
    g_ctx = start(&boot_desc, 31u);
    TT_ASSERT_NOT_NULL(g_ctx);
    /* 外部线程没有"当前上下文"：槽位归属无从判断，直接拒绝。 */
    TT_ASSERT_INT(CM_ERR_STATE, res_acquire(&g_ctx->pub.slots[2], RES_KIND_MEM, 8u, RES_LIFE_APP));
    TT_ASSERT_TRUE(g_ctx->pub.slots[2] == NULL);
    res_release(&g_ctx->pub.slots[2]); /* 空槽位：无操作 */
    stop_all();
}

TT_CASE(release_then_reuse_the_same_slot)
{
    setup();
    g_ctx = start(&reuse_desc, 32u);
    TT_ASSERT_NOT_NULL(g_ctx);
    wait_for(&g_reuse_ok, 1);
    TT_ASSERT_INT(1, g_reuse_ok); /* release 之后同一个槽位能再 acquire */

    fw_app_request_cancel(g_ctx);
    for (int i = 0; i < 400; i++) {
        if (fw_app_state(g_ctx) == FW_APP_STATE_STOPPED) {
            break;
        }
        pf_host_fake_ops_get()->sleep_ms(2u);
    }
    /* 第一个已被 release、第二个由退出路径回收：恰好一次。 */
    TT_ASSERT_INT(1, (int)fw_app_slots_released(g_ctx));
    TT_ASSERT_TRUE(g_ctx->pub.slots[0] == NULL);
    stop_all();
}

int main(void)
{
    TT_PROGRAM("test_res");
    TT_RUN(exit_releases_app_lifetime_slots_exactly_once);
    TT_RUN(boot_lifetime_slot_survives_exit);
    TT_RUN(acquire_outside_an_app_task_is_state);
    TT_RUN(release_then_reuse_the_same_slot);
    return tt_summary();
}
