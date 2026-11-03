/* tests/app/test_demo_lifecycle.c —— 步骤 3.2 的验收：App 闭环。
 *
 * 这是"App 闭环第一次真正成立"的那组断言。前面的用例测的都是零件：
 * 注册表、伪阻塞、退出路径；这一份测的是**拼起来之后还成不成立**：
 *
 *   写清单 → fw_init 收下 → fw_boot 拉起 → init 跑了 → 主任务转圈 →
 *   请求停止 → 进 STOPPED → exit 恰好一次 → 管理器用新 id 重新拉起
 *
 * 最后一步是 D18 的直接验收：**重启不复用旧 id**。所以这里刻意断言
 * `new_id != old_id`，而不是只断言"又能跑起来"——后者在 id 被复用的情况下
 * 也会通过，而那一天正是"拿着旧 id 的代码操作到新实例上"的 bug 起点。
 *
 * 时间用 `clock_fake`：样板 App 每圈 100ms，假时钟下几十圈也就几毫秒。
 */
#include "tinytest.h"
#include "tests/support/clock_fake.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

#include "apps/app_manifest.h"
#include "apps/demo/app_demo.h"

/* 用真表的时间等条件成立；超时返回 0（tinytest 没有超时保护，这里自带）。 */
static int wait_until(int (*pred)(void), uint32_t timeout_ms)
{
    const pf_ops_t *real = pf_host_fake_ops_get();
    uint32_t t0 = real->clock_ms();

    while (!pred()) {
        if ((uint32_t)(real->clock_ms() - t0) > timeout_ms) {
            return 0;
        }
        real->sleep_ms(1u);
    }
    return 1;
}

static int demo_has_ticked(void)
{
    return app_demo_ticks() > 0u;
}

static int demo_stopped(void)
{
    return fw_app_state(fw_live_at(0u)) == FW_APP_STATE_STOPPED;
}

/* 等一个实例进终态（STOPPED 或 FAILED）。 */
static int wait_terminal(struct fw_app_ctx *c, uint32_t timeout_ms)
{
    const pf_ops_t *real = pf_host_fake_ops_get();
    uint32_t t0 = real->clock_ms();

    while (fw_app_state(c) != FW_APP_STATE_STOPPED &&
           fw_app_state(c) != FW_APP_STATE_FAILED) {
        if ((uint32_t)(real->clock_ms() - t0) > timeout_ms) {
            return 0;
        }
        real->sleep_ms(1u);
    }
    return 1;
}

/* 收尾：把还活着的实例停掉，再清注册表。
 *
 * **必须停干净**：`fw_reset()` 只清框架的表，停不掉已经跑起来的任务。
 * 漏一个，下一条用例就是在"上一个 App 还在转圈"的世界里跑——计数会互相污染，
 * 断言看起来像框架坏了。 */
static void stop_and_reset(void)
{
    uint32_t i;

    for (i = 0; i < fw_live_count(); i++) {
        fw_app_request_cancel(fw_live_at(i));
    }
    for (i = 0; i < fw_live_count(); i++) {
        (void)wait_terminal(fw_live_at(i), 2000u);
    }
    fw_reset();
}

static void boot_workspace(void)
{
    fw_reset();
    clock_fake_install(pf_host_fake_ops_get());
    clock_fake_set_sleep_hook(NULL);
    app_demo_reset_counters();
}

/* ── 用例 ─────────────────────────────────────────────────────── */

/* 清单 → init → boot：描述体被收下，样板 App 真的跑起来了。 */
TT_CASE(manifest_is_registered_and_booted)
{
    boot_workspace();
    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
    TT_ASSERT_INT(1u, fw_app_count());
    TT_ASSERT_INT(0u, fw_live_count()); /* 还没 boot：注册不等于启动 */

    TT_ASSERT_INT(CM_OK, fw_boot());
    TT_ASSERT_INT(1u, fw_live_count());

    if (!wait_until(demo_has_ticked, 2000u)) {
        TT_ASSERT_INT(1, app_demo_ticks() > 0u);
    }
    TT_ASSERT_INT(1u, app_demo_inits());
    TT_ASSERT_INT(FW_APP_STATE_RUNNING, fw_app_state(fw_live_at(0u)));
    TT_ASSERT_INT(2u, fw_live_at(0u)->pub.id); /* 清单里写的 id */
    stop_and_reset();
}

/* 请求停止 → STOPPED，`exit()` 恰好一次；主任务不再转圈。 */
TT_CASE(stop_goes_stopped_and_exits_once)
{
    boot_workspace();
    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
    TT_ASSERT_INT(CM_OK, fw_boot());
    TT_ASSERT_TRUE(wait_until(demo_has_ticked, 2000u));

    fw_app_request_cancel(fw_live_at(0u));
    if (!wait_until(demo_stopped, 2000u)) {
        TT_ASSERT_INT(FW_APP_STATE_STOPPED, fw_app_state(fw_live_at(0u)));
    }
    TT_ASSERT_INT(1u, app_demo_exits());

    {
        uint32_t frozen = app_demo_ticks();
        const pf_ops_t *real = pf_host_fake_ops_get();

        real->sleep_ms(20u); /* 真等 20ms：任务已经结束，计数不该再涨 */
        TT_ASSERT_INT(frozen, app_demo_ticks());
    }
    stop_and_reset();
}

/* D18：重启 = 新建上下文 + **新 id**，旧 id 不复用。 */
TT_CASE(restart_uses_a_fresh_id)
{
    struct fw_app_ctx *second;
    uint32_t old_id;
    uint32_t new_id = 0u;
    uint32_t ticks_before;

    boot_workspace();
    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
    TT_ASSERT_INT(CM_OK, fw_boot());
    TT_ASSERT_TRUE(wait_until(demo_has_ticked, 2000u));

    old_id = fw_live_at(0u)->pub.id;
    fw_app_request_cancel(fw_live_at(0u));
    TT_ASSERT_TRUE(wait_until(demo_stopped, 2000u));
    ticks_before = app_demo_ticks();

    TT_ASSERT_INT(CM_OK, fw_alloc_id(&new_id));
    TT_ASSERT_TRUE(new_id != old_id); /* 这条就是 D18 本身 */
    TT_ASSERT_TRUE(new_id >= APP_ID_MIN && new_id <= APP_ID_MAX);

    second = fw_app_ctx_new_with_id(&app_demo_desc, new_id);
    TT_ASSERT_NOT_NULL(second);
    TT_ASSERT_INT(CM_OK, fw_app_start(second));
    {
        const pf_ops_t *real = pf_host_fake_ops_get();
        uint32_t t0 = real->clock_ms();

        /* 不能只看 ticks>0：那是第一个实例留下的数，会立刻为真。 */
        while (!(fw_app_state(second) == FW_APP_STATE_RUNNING && app_demo_inits() >= 2u)) {
            if ((uint32_t)(real->clock_ms() - t0) > 2000u) {
                break;
            }
            real->sleep_ms(1u);
        }
    }

    TT_ASSERT_INT(FW_APP_STATE_RUNNING, fw_app_state(second));
    TT_ASSERT_INT(2u, app_demo_inits());   /* 第二次 init 也是真的 */
    TT_ASSERT_TRUE(app_demo_ticks() > ticks_before);
    TT_ASSERT_INT(new_id, second->pub.id);

    fw_app_request_cancel(second);
    {
        const pf_ops_t *real = pf_host_fake_ops_get();
        uint32_t t0 = real->clock_ms();

        while (fw_app_state(second) != FW_APP_STATE_STOPPED) {
            if ((uint32_t)(real->clock_ms() - t0) > 2000u) {
                break;
            }
            real->sleep_ms(1u);
        }
        TT_ASSERT_INT(FW_APP_STATE_STOPPED, fw_app_state(second));
        TT_ASSERT_INT(2u, app_demo_exits());
    }
    fw_app_ctx_free(second);
    stop_and_reset();
}

/* 清单里的 id 已经被占用：发号器从 3 开始，不会把 2 再发一次。 */
TT_CASE(id_allocator_skips_registered_ids)
{
    uint32_t a = 0u;
    uint32_t b = 0u;

    boot_workspace();
    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));

    TT_ASSERT_INT(CM_OK, fw_alloc_id(&a));
    TT_ASSERT_INT(CM_OK, fw_alloc_id(&b));
    TT_ASSERT_INT(3u, a);
    TT_ASSERT_INT(4u, b);
    stop_and_reset();
}

/* boot 的前后条件：没 init 不行，boot 两次不行。 */
TT_CASE(boot_requires_init_and_happens_once)
{
    boot_workspace();
    TT_ASSERT_INT(CM_ERR_STATE, fw_boot()); /* 未 bind/init */

    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
    TT_ASSERT_INT(CM_OK, fw_boot());
    TT_ASSERT_INT(CM_ERR_STATE, fw_boot()); /* 第二次会把同一批起两遍 */
    stop_and_reset();
}

/* D19：管理器负责回收——先摘表、再释放，顺序写反就是野指针。 */
TT_CASE(manager_reaps_stopped_instance)
{
    struct fw_app_ctx *c;
    uint32_t ticks;

    boot_workspace();
    TT_ASSERT_INT(CM_OK, fw_platform_bind(clock_fake_ops()));
    TT_ASSERT_INT(CM_OK, fw_init(&apps_manifest, 1u));
    TT_ASSERT_INT(CM_OK, fw_boot());
    TT_ASSERT_TRUE(wait_until(demo_has_ticked, 2000u));

    c = fw_live_at(0u);
    TT_ASSERT_NOT_NULL(c);
    fw_app_request_cancel(c);
    TT_ASSERT_TRUE(wait_terminal(c, 2000u));

    /* ① 先摘表：表里不再有它，管理器再也拿不到这个指针 */
    fw_live_remove(c);
    TT_ASSERT_INT(0u, fw_live_count());
    TT_ASSERT_TRUE(fw_live_at(0u) == NULL);
    /* 摘表不影响实例本身：上下文还活着、状态还在 */
    TT_ASSERT_INT(FW_APP_STATE_STOPPED, fw_app_state(c));

    /* ② 再释放：这一步之后 `c` 就是野指针，测试不再碰它 */
    ticks = app_demo_ticks();
    fw_app_ctx_free(c);
    {
        const pf_ops_t *real = pf_host_fake_ops_get();

        real->sleep_ms(20u);
        TT_ASSERT_INT(ticks, app_demo_ticks()); /* 任务确实早结束了 */
    }
    fw_reset();
}

int main(void)
{
    TT_PROGRAM("test_demo_lifecycle");

    TT_RUN(manifest_is_registered_and_booted);
    TT_RUN(stop_goes_stopped_and_exits_once);
    TT_RUN(restart_uses_a_fresh_id);
    TT_RUN(id_allocator_skips_registered_ids);
    TT_RUN(boot_requires_init_and_happens_once);
    TT_RUN(manager_reaps_stopped_instance);
    return tt_summary();
}
