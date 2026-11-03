/* tests/framework/test_fw_registry.c —— 步骤 2.3 的验收：注册表、查重、清单合并。
 *
 * 这里驱动的是**真框架**（`fw_platform_bind()` + `fw_init()`），不是 1.4 里那份
 * 测试侧检查器——1.4 的"诚实边界"就是等这一步把语义搬进 `fw_init()`。
 *
 * 每个用例开头都 `fw_reset()`：注册表是全局单例（整机只有一次 init），
 * 不重置的话第二条用例会撞上 `CM_ERR_STATE`，测试就变成测顺序而不是测行为。
 */
#include "tinytest.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"

/* ── 三个回调：内容不重要，非 NULL 才重要 ─────────────────────── */
static cm_err_t cb_init(app_ctx_t *ctx)
{
    (void)ctx;
    return CM_OK;
}

static void cb_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static void cb_task(app_ctx_t *ctx)
{
    (void)ctx;
}

/* 字段顺序必须与 app_desc_t 一致：id, name, init, exit, task, flags, stack_bytes */
#define DESC(ident, nm) \
    { (ident), (nm), cb_init, cb_exit, cb_task, 0u, APP_STACK_MIN }

static const app_desc_t app_a = DESC(2u, "a");
static const app_desc_t app_b = DESC(3u, "b");
static const app_desc_t app_c = DESC(4u, "c");

static const app_desc_t *const list_ab[] = { &app_a, &app_b };
static const app_desc_t *const list_c[] = { &app_c };

/* 平台表副本：用来注入"坏表"，不改动 host-fake 自己那张真表。 */
static pf_ops_t bad_ops;

static void use_good_platform(void)
{
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
}

/* ── 平台表校验 ───────────────────────────────────────────────── */

TT_CASE(bind_accepts_host_fake)
{
    fw_reset();
    TT_ASSERT_INT(CM_OK, fw_platform_bind(pf_host_fake_ops_get()));
    TT_ASSERT_NOT_NULL(fw_ops());
}

TT_CASE(bind_rejects_null_table)
{
    fw_reset();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_platform_bind(NULL));
    TT_ASSERT_TRUE(fw_ops() == NULL);
}

TT_CASE(bind_rejects_version_mismatch)
{
    fw_reset();
    bad_ops = *pf_host_fake_ops_get();
    bad_ops.version = PF_OPS_VERSION + 1u;
    TT_ASSERT_INT(CM_ERR_VERSION, fw_platform_bind(&bad_ops));
}

TT_CASE(bind_rejects_missing_member)
{
    fw_reset();
    bad_ops = *pf_host_fake_ops_get();
    bad_ops.task_create = NULL;
    TT_ASSERT_INT(CM_ERR_INVALID, fw_platform_bind(&bad_ops));
}

/* host-fake 的 caps 恒 0，所以要"撒谎"必须换一个 caps 函数进来。 */
static uint32_t caps_with_reserved_bit(void)
{
    return (uint32_t)(1u << 10); /* 10..31 是保留位，读 1 = 这块板在未来 */
}

TT_CASE(bind_rejects_reserved_caps)
{
    fw_reset();
    bad_ops = *pf_host_fake_ops_get();
    bad_ops.caps = caps_with_reserved_bit;
    TT_ASSERT_INT(CM_ERR_INVALID, fw_platform_bind(&bad_ops));
}

TT_CASE(bind_twice_is_state_error)
{
    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_STATE, fw_platform_bind(pf_host_fake_ops_get()));
}

/* ── fw_init 前置条件 ─────────────────────────────────────────── */

TT_CASE(init_without_bind_is_state_error)
{
    const fw_manifest_t m = { list_ab, 2u };

    fw_reset();
    TT_ASSERT_INT(CM_ERR_STATE, fw_init(&m, 1u));
}

TT_CASE(init_null_manifests_is_invalid)
{
    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(NULL, 1u));
}

TT_CASE(init_zero_count_is_invalid)
{
    const fw_manifest_t m = { list_ab, 2u };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&m, 0u));
}

/* ── 合并 ─────────────────────────────────────────────────────── */

TT_CASE(merge_two_manifests)
{
    const fw_manifest_t ms[2] = { { list_ab, 2u }, { list_c, 1u } };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_OK, fw_init(ms, 2u));
    TT_ASSERT_INT(3, (int)fw_app_count());
    TT_ASSERT_INT(2, (int)fw_app_at(0u)->id);
    TT_ASSERT_INT(3, (int)fw_app_at(1u)->id);
    TT_ASSERT_INT(4, (int)fw_app_at(2u)->id);
    TT_ASSERT_TRUE(fw_app_at(3u) == NULL);
}

TT_CASE(empty_manifest_is_legal_but_not_whole_boot)
{
    /* 一份清单是空的、另一份有 App：合法（某个模块这次没编进来）。 */
    const fw_manifest_t empty = { NULL, 0u };
    const fw_manifest_t full = { list_c, 1u };
    const fw_manifest_t ms[2] = { empty, full }; /* NOLINT：故意先空后有 */

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_OK, fw_init(ms, 2u));
    TT_ASSERT_INT(1, (int)fw_app_count());
}

TT_CASE(all_manifests_empty_is_invalid)
{
    const fw_manifest_t empty = { NULL, 0u };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&empty, 1u));
}

/* ── 查重 ─────────────────────────────────────────────────────── */

TT_CASE(duplicate_id_across_manifests_blocks_boot)
{
    const app_desc_t *const dup[] = { &app_a }; /* 同一份 a 再登记一次 */
    const fw_manifest_t ms[2] = { { list_ab, 2u }, { dup, 1u } };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(ms, 2u));
    TT_ASSERT_INT(0, (int)fw_app_count()); /* 失败必须回滚成空表 */
}

/* ── 单条目合法性 ─────────────────────────────────────────────── */

TT_CASE(id_zero_is_invalid)
{
    const app_desc_t bad = DESC(APP_ID_INVALID, "bad0");
    const app_desc_t *const one[] = { &bad };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&(fw_manifest_t){ one, 1u }, 1u));
}

TT_CASE(id_manager_is_invalid_for_normal_app)
{
    const app_desc_t bad = DESC(APP_ID_MANAGER, "bad1");
    const app_desc_t *const one[] = { &bad };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&(fw_manifest_t){ one, 1u }, 1u));
}

TT_CASE(missing_task_is_invalid)
{
    const app_desc_t bad = { 5u, "bad5", cb_init, cb_exit, NULL, 0u, APP_STACK_MIN };
    const app_desc_t *const one[] = { &bad };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&(fw_manifest_t){ one, 1u }, 1u));
}

TT_CASE(unknown_flag_is_invalid)
{
    const app_desc_t bad = { 6u, "bad6", cb_init, cb_exit, cb_task, (1u << 7), APP_STACK_MIN };
    const app_desc_t *const one[] = { &bad };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&(fw_manifest_t){ one, 1u }, 1u));
}

TT_CASE(small_stack_is_invalid)
{
    const app_desc_t bad = { 7u, "bad7", cb_init, cb_exit, cb_task, 0u, APP_STACK_MIN - 1u };
    const app_desc_t *const one[] = { &bad };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&(fw_manifest_t){ one, 1u }, 1u));
}

TT_CASE(null_slot_is_invalid)
{
    const app_desc_t *const one[] = { NULL };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_ERR_INVALID, fw_init(&(fw_manifest_t){ one, 1u }, 1u));
}

/* ── 二次初始化 ───────────────────────────────────────────────── */

TT_CASE(second_init_is_state_error)
{
    const fw_manifest_t m = { list_ab, 2u };

    fw_reset();
    use_good_platform();
    TT_ASSERT_INT(CM_OK, fw_init(&m, 1u));
    TT_ASSERT_INT(CM_ERR_STATE, fw_init(&m, 1u));
}

/* ── 与测试侧模型交叉验证 ─────────────────────────────────────── */

/* tests/platform/test_pf_conformance.c 里有一份**独立**的契约检查器
 * （`pf_contract_check()`），它故意不 include framework/，只靠 pf_ops.h 就能判。
 * 两份实现应当给出同一结论；这里把它们摆在一起比一次——不是重复，是防止
 * 两边各自漂移而没人发现（框架松一档 = 考卷比真题简单）。 */
TT_CASE(ops_check_agrees_with_platform_model)
{
    pf_ops_t bad;

    TT_ASSERT_INT(CM_OK, fw_ops_check(pf_host_fake_ops_get()));

    bad = *pf_host_fake_ops_get();
    bad.version = PF_OPS_VERSION + 1u;
    TT_ASSERT_INT(CM_ERR_VERSION, fw_ops_check(&bad));

    bad = *pf_host_fake_ops_get();
    bad.caps = caps_with_reserved_bit;
    TT_ASSERT_INT(CM_ERR_INVALID, fw_ops_check(&bad));

    bad = *pf_host_fake_ops_get();
    bad.mutex_give = NULL;
    TT_ASSERT_INT(CM_ERR_INVALID, fw_ops_check(&bad));

    TT_ASSERT_INT(CM_ERR_INVALID, fw_ops_check(NULL));
}

int main(void)
{
    TT_PROGRAM("test_fw_registry");
    TT_RUN(bind_accepts_host_fake);
    TT_RUN(bind_rejects_null_table);
    TT_RUN(bind_rejects_version_mismatch);
    TT_RUN(bind_rejects_missing_member);
    TT_RUN(bind_rejects_reserved_caps);
    TT_RUN(bind_twice_is_state_error);
    TT_RUN(init_without_bind_is_state_error);
    TT_RUN(init_null_manifests_is_invalid);
    TT_RUN(init_zero_count_is_invalid);
    TT_RUN(merge_two_manifests);
    TT_RUN(empty_manifest_is_legal_but_not_whole_boot);
    TT_RUN(all_manifests_empty_is_invalid);
    TT_RUN(duplicate_id_across_manifests_blocks_boot);
    TT_RUN(id_zero_is_invalid);
    TT_RUN(id_manager_is_invalid_for_normal_app);
    TT_RUN(missing_task_is_invalid);
    TT_RUN(unknown_flag_is_invalid);
    TT_RUN(small_stack_is_invalid);
    TT_RUN(null_slot_is_invalid);
    TT_RUN(second_init_is_state_error);
    TT_RUN(ops_check_agrees_with_platform_model);
    return tt_summary();
}
