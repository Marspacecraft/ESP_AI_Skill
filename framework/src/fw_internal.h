/* framework/src/fw_internal.h —— 框架内部接口（**不是 App 的词汇表**）。
 *
 * 为什么放在 `src/` 而不是 `include/`：`framework/include/` 是 tag 1/2 的
 * link 面（见 policy.txt），那里出现 `pf_ops_t` 就等于把平台表塞进 App 的
 * 编译单元，`fw_app.h` 那条"只准 include common/"的硬约束当场作废。
 * 所以凡是签名里带 `pf_ops_t` / `pf_manifest` / 注册表的东西，一律住这里。
 *
 * 谁能 include 它：框架自己的 .c 源文件，以及 tests/framework/ 下的测试（测试要
 * 直接驱动真框架，见 1.4 的"诚实边界"）。App、平台、api 都不该看见它。
 *
 * 线程与中断：本文件里的函数全部只能在框架初始化阶段（单线程、无并发）调用；
 * 没有任何一个是可重入或 ISR 安全的。
 */
#ifndef ESP_AI_SKILL_FW_INTERNAL_H
#define ESP_AI_SKILL_FW_INTERNAL_H

#include "common/include/cm_types.h"
#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 平台表 ────────────────────────────────────────────────────── */

/** 校验一张平台表是否可被框架接受。
 *
 * @param ops 平台表；不得为 NULL。
 * @return `CM_OK` 可用；`CM_ERR_INVALID` 表不完整（九项有 NULL）或 caps 保留位
 *         非 0；`CM_ERR_VERSION` 版本不匹配。失败时*不*保留任何状态。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：会**调用** `ops->caps()`，因此调用前该表必须已经填好。
 */
cm_err_t fw_ops_check(const pf_ops_t *ops);

/** 绑定平台表。整机只能绑一次，且必须早于 `fw_init()`。
 *
 * @param ops 平台表；生命周期必须覆盖整个运行期（框架只存指针，不拷贝）。
 * @return `CM_OK`；`CM_ERR_STATE` 已绑过或已 `fw_init()`；其余同 `fw_ops_check()`。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：绑定失败不会留下半绑定状态。
 */
cm_err_t fw_platform_bind(const pf_ops_t *ops);

/** 取已绑定的平台表。未绑定时返回 NULL。
 *  线程与中断：初始化阶段与运行期均可读；返回的指针只读。 */
const pf_ops_t *fw_ops(void);

/* ── 注册表 ────────────────────────────────────────────────────── */

/** 合并 N 份清单进注册表并校验。
 *
 * @param manifests 清单数组；为 NULL 或 `count==0` 返回 `CM_ERR_INVALID`。
 * @param count     清单份数。
 * @return `CM_OK` 注册完成；`CM_ERR_STATE` 未绑平台表或已初始化过；
 *         `CM_ERR_INVALID` 任一条目不合法（见下）或**所有清单合计为 0 个 App**。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：失败时注册表回到空（不会留下半份清单）。单条目合法条件：
 *                   id ∈ [2,254]、`name`/`init`/`exit`/`task` 非 NULL、
 *                   `flags` 只有 `APP_FLAG_SYSTEM`、`stack_bytes >= APP_STACK_MIN`、
 *                   **id 不得重复**。空清单（`count==0` 的那一份）合法，跳过。
 */
cm_err_t fw_init(const fw_manifest_t *manifests, uint32_t count);

/** 已注册的 App 个数。未初始化时返回 0。
 *  线程与中断：初始化后可读；ISR 里读到的是稳定的常量。 */
uint32_t fw_app_count(void);

/** 按注册顺序取第 index 个描述体；越界返回 NULL。
 *  线程与中断：初始化后可读；返回的指针指向静态常量，不要改。 */
const app_desc_t *fw_app_at(uint32_t index);

/** 清空注册表与平台绑定，回到"未初始化"。**只给测试用**——真机上没有第二次启动。
 *  线程与中断：无任务运行时才可调。 */
void fw_reset(void);

/* ── App 运行期上下文与伪阻塞 ──────────────────────────────────── */

/** 伪阻塞的单片长度上限（毫秒）。架构约束是"单段不可打断等待 < 200ms"，
 *  框架取 20ms——留 10 倍余量给平台调度抖动。 */
#define FW_DELAY_SLICE_MS 20u

/** App 的六个状态。`EMPTY` 只在上下文刚分配、任务还没起来时出现。 */
typedef enum fw_app_state_e {
    FW_APP_STATE_EMPTY = 0,
    FW_APP_STATE_INIT,
    FW_APP_STATE_RUNNING,
    FW_APP_STATE_STOPPING,
    FW_APP_STATE_STOPPED,
    FW_APP_STATE_FAILED
} fw_app_state_e;

/** 框架侧的 App 上下文。App 拿到的 `app_ctx_t*` 就是它的 `pub` 成员。
 *
 * `cancel` / `state` / `exit_calls` / `in_exit` 是**跨任务可见**的：任务自己写，
 * 别的任务（管理器、测试、退出路径）读。所以它们必须 volatile，且只能用
 * "写一个 32 位整数"的表达——没有锁，也不该有锁。
 */
struct fw_app_ctx {
    app_ctx_t pub;                  /* 必须是第一个成员：App 拿到的是 &pub */
    const app_desc_t *desc;
    volatile uint32_t cancel;       /* 取消位：1 = 有停止请求 */
    volatile uint32_t state;        /* fw_app_state_e */
    volatile uint32_t exit_calls;   /* exit() 被调用过几次（只该是 0 或 1） */
    volatile uint32_t in_exit;      /* 1 = 正在跑 exit()：取消位失效 */
};

/** 分配一个上下文并做静态初始化；**不创建任务**。
 *
 * @param desc 注册表里那份描述体；不得为 NULL，`desc->id` 已在 `fw_init()` 校验过。
 * @return 上下文指针；平台表未绑或内存不足返回 NULL。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：用 `fw_ops()->mem_alloc` 分配，必须 `fw_app_ctx_free()` 释放；
 *                   失败时不留任何状态。
 */
struct fw_app_ctx *fw_app_ctx_new(const app_desc_t *desc);

/** 释放上下文。**只能在任务已经结束之后调**（任务还在跑就释放 = UAF）。 */
void fw_app_ctx_free(struct fw_app_ctx *c);

/** 为上下文创建任务，任务里依次跑 `init()` → `task()`。
 *
 * @param c `fw_app_ctx_new()` 的产物。
 * @return `CM_OK` 任务已创建；`CM_ERR_NOMEM` 平台拒绝建任务；`CM_ERR_STATE`
 *         `c` 为 NULL 或已经启动过。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：任务入口会把 `state` 推成 INIT→RUNNING；`init()` 失败或
 *                   `task()` 返回都会走退出路径（`fw_run_exit`），那条路径
 *                   **不返回**，所以这里看到的 `state` 之后还会变。
 */
cm_err_t fw_app_start(struct fw_app_ctx *c);

/** 当前上下文的状态（`fw_app_state_e`）。`c` 为 NULL 时返回 `FW_APP_STATE_EMPTY`。 */
fw_app_state_e fw_app_state(const struct fw_app_ctx *c);

/** `exit()` 至今被调用过几次。正常是 1（含启动失败）。 */
uint32_t fw_app_exit_calls(const struct fw_app_ctx *c);

/** 置取消位。可以在别的任务里调，也可以在测试里调。
 *  线程与中断：可以跨任务；ISR 里能调（只写一个整数），但不在 v1 承诺范围内。 */
void fw_app_request_cancel(struct fw_app_ctx *c);

/** 取**当前任务**的上下文；不在 App 任务里时返回 NULL。
 *  线程与中断：靠 C11 `_Thread_local` 实现，每个任务各一份；ISR 里返回 NULL。 */
struct fw_app_ctx *fw_current_ctx(void);

/** 走完退出路径并结束当前任务——**永不返回**。
 *
 * @param c         当前任务的上下文。
 * @param end_state 终态：`STOPPED`（被取消/被停止）或 `FAILED`（init 失败或
 *                  task 返回）。
 *         线程与中断：只能在 App 任务里调；ISR 禁止。
 *         注意事项：五步顺序是契约——①先清取消位（否则 exit() 里的阻塞会再
 *                   命中一次、永不结束）②`exit()` 恰好调用一次
 *                   ③（阶段 6 才有的资源回收）④置终态 ⑤结束任务。
 */
void fw_run_exit(struct fw_app_ctx *c, fw_app_state_e end_state)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noreturn))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_FW_INTERNAL_H */
