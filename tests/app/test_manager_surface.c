/* tests/app/test_manager_surface.c —— 管理器面（`api/af_api/mgr.h`）的可用性验收。
 *
 * 为什么需要这一份：`fw_alloc_id()` / `fw_app_ctx_new*()` / `fw_app_start()` /
 * `fw_app_state()` / `fw_live_*()` / `fw_app_at()` 以前只声明在
 * `framework/src/fw_internal.h`，而任何 App 的 link 集合都不含 `framework/src/`
 * ——**文档说"管理器拉起业务 App"，代码里却没人能当管理器**。这一份钉住修复后的
 * 事实：只用公开头就能写出一段完整的管理器逻辑。
 *
 * 两个证据分工不同，缺一不可：
 *   - 这里：写得出来、跑得通（行为）。
 *   - `tools/check_link.py`：`api/af_api/mgr.h` 的依赖闭包不含 `framework/src/`
 *     与 `platform/`（结构）。这一条才是"App 侧真能 include"的机器判据。
 *
 * 本文件**不 include `framework/src/fw_internal.h`**：一旦有人把公开面又搬回内部头，
 * 这里会先编译不过。启动走公开的 `fw_embed_start()`，不再需要内部 `fw_init`/`fw_boot`。
 */
#include "tinytest.h"

#include "api/af_api/app.h"
#include "api/af_api/mgr.h"
#include "framework/include/fw_embed.h"
#include "platform/port/pf_ops.h"

#include "apps/app_manifest.h"
#include "apps/demo/app_demo.h"

/* 真表的时间：本用例不装假时钟，因为管理器本来就要在真实时间里观察别人。 */
static const pf_ops_t *ops(void)
{
    return pf_platform_ops();
}

static int wait_state(struct fw_app_ctx *c, fw_app_state_e want, uint32_t timeout_ms)
{
    uint32_t t0 = ops()->clock_ms();

    while (fw_app_state(c) != want) {
        if ((uint32_t)(ops()->clock_ms() - t0) > timeout_ms) {
            return 0;
        }
        ops()->sleep_ms(1u);
    }
    return 1;
}

static int wait_terminal(struct fw_app_ctx *c, uint32_t timeout_ms)
{
    uint32_t t0 = ops()->clock_ms();

    while (fw_app_state(c) != FW_APP_STATE_STOPPED && fw_app_state(c) != FW_APP_STATE_FAILED) {
        if ((uint32_t)(ops()->clock_ms() - t0) > timeout_ms) {
            return 0;
        }
        ops()->sleep_ms(1u);
    }
    return 1;
}

/* 某个实例是否还在活实例表里（按指针相等找）。 */
static int found_at_live(struct fw_app_ctx *c)
{
    uint32_t i;

    for (i = 0; i < fw_live_count(); i++) {
        if (fw_live_at(i) == c) {
            return 1;
        }
    }
    return 0;
}

/* 整机只启动一次（`fw_embed_start()` 是"只发一次机会"的），用例之间共用。 */
static int g_started;

static void ensure_started(void)
{
    if (g_started == 0) {
        TT_ASSERT_INT(CM_OK, fw_embed_start(&apps_manifest));
        g_started = 1;
    }
}

/* ── 用例 ─────────────────────────────────────────────────────── */

/* 公开的嵌入入口能拉起 SYSTEM App，管理器面能看到它。 */
TT_CASE(embed_start_brings_up_the_system_app)
{
    ensure_started();

    TT_ASSERT_INT(1u, fw_live_count());
    TT_ASSERT_TRUE(wait_state(fw_live_at(0u), FW_APP_STATE_RUNNING, 2000u));
    TT_ASSERT_INT(2u, fw_app_id(fw_live_at(0u)));
}

/* 清单里有几个描述体、分别是谁——管理器靠它决定"可以起谁"。 */
TT_CASE(manifest_is_visible_through_the_manager_surface)
{
    ensure_started();

    TT_ASSERT_INT(1u, fw_app_count());
    TT_ASSERT_TRUE(fw_app_at(0u) == &app_demo_desc);
    TT_ASSERT_TRUE(fw_app_at(1u) == NULL); /* 越界给 NULL，不给垃圾 */
}

/* 发号器：只发没被用过的号，且不复用。清单里 demo 占了 2，所以从 3 开始。 */
TT_CASE(allocator_hands_out_unused_ids)
{
    uint32_t a = 0u;
    uint32_t b = 0u;

    ensure_started();

    TT_ASSERT_INT(CM_OK, fw_alloc_id(&a));
    TT_ASSERT_INT(CM_OK, fw_alloc_id(&b));
    TT_ASSERT_INT(3u, a);
    TT_ASSERT_INT(4u, b);
    TT_ASSERT_TRUE(a != b);
}

/* 全链路：发号 → 指定 id 造实例 → 启动 → 观察 → 取消 → 终态 → 摘表 → 释放。 */
TT_CASE(manager_starts_stops_and_reaps_a_child)
{
    uint32_t id = 0u;
    uint32_t before = app_demo_ticks();
    struct fw_app_ctx *child;

    ensure_started();
    TT_ASSERT_INT(CM_OK, fw_alloc_id(&id));

    child = fw_app_ctx_new_with_id(&app_demo_desc, id);
    TT_ASSERT_TRUE(child != NULL);
    TT_ASSERT_INT(FW_APP_STATE_EMPTY, fw_app_state(child));

    TT_ASSERT_INT(CM_OK, fw_app_start(child));
    TT_ASSERT_TRUE(wait_state(child, FW_APP_STATE_RUNNING, 2000u));
    TT_ASSERT_INT(id, fw_app_id(child));
    TT_ASSERT_INT(2u, fw_live_count()); /* D53：管理器拉起的孩子也在表里 */

    /* 让它跑几圈（样板 App 每圈 100ms）。 */
    ops()->sleep_ms(250u);
    TT_ASSERT_TRUE(app_demo_ticks() > before);

    fw_app_request_cancel(child);
    TT_ASSERT_TRUE(wait_terminal(child, 2000u));
    TT_ASSERT_INT(FW_APP_STATE_STOPPED, fw_app_state(child));
    TT_ASSERT_INT(1u, fw_app_exit_calls(child)); /* exit() 恰好一次 */

    /* D53：停掉的实例**仍然留在表里**，直到管理器认领（摘表）。所以这里先看到 2，
     * 摘表后回到 1。顺序反过来（先 free 再 remove）会在表里留一根悬垂指针（D19）。 */
    TT_ASSERT_INT(2u, fw_live_count());
    fw_live_remove(child);
    TT_ASSERT_INT(1u, fw_live_count());
    fw_app_ctx_free(child);
}

/* 表里的每一项都要能被管理器找到、并按顺序停下来——这是"没有孤儿实例"的证据。 */
TT_CASE(live_table_lists_every_started_instance)
{
    uint32_t id = 0u;
    struct fw_app_ctx *child;

    ensure_started();
    TT_ASSERT_INT(CM_OK, fw_alloc_id(&id));
    child = fw_app_ctx_new_with_id(&app_demo_desc, id);
    TT_ASSERT_TRUE(child != NULL);
    TT_ASSERT_INT(CM_OK, fw_app_start(child));
    TT_ASSERT_TRUE(wait_state(child, FW_APP_STATE_RUNNING, 2000u));

    /* 管理器造的孩子也在表里（D53），所以"丢失指针"不再等于"再也找不到"。 */
    TT_ASSERT_INT(1, found_at_live(child));

    fw_app_request_cancel(child);
    TT_ASSERT_TRUE(wait_terminal(child, 2000u));
    /* 停掉之后仍在表里（可观测），回收要显式两步：摘表 → 释放。 */
    TT_ASSERT_INT(1, found_at_live(child));
    fw_live_remove(child);
    TT_ASSERT_TRUE(fw_live_at(fw_live_count()) == NULL); /* 越界给 NULL */
    fw_app_ctx_free(child);
}

int main(void)
{
    tt_set_program("test_manager_surface");

    TT_RUN(embed_start_brings_up_the_system_app);
    TT_RUN(manifest_is_visible_through_the_manager_surface);
    TT_RUN(allocator_hands_out_unused_ids);
    TT_RUN(manager_starts_stops_and_reaps_a_child);
    TT_RUN(live_table_lists_every_started_instance);

    return tt_summary();
}
