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

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_FW_INTERNAL_H */
