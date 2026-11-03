/* framework/src/fw_ops_check.c —— 平台表能不能被框架接受（步骤 2.3 的前半）。
 *
 * 这份判定原来是 1.4 里测试侧的一份私有实现（`pf_contract_check()`）。现在
 * 搬进框架：**同一个判定只能有一处**，否则测试绿了而 `fw_init()` 松一档，
 * 就成了"考卷比真题简单"。
 *
 * 判定顺序刻意这样排：版本 → 完整性 → caps。理由：版本不对时，表里哪些字段
 * 存在都说不准，再去逐项判 NULL 只是在解读一份布局未知的内存。
 */
#include "framework/src/fw_internal.h"

cm_err_t fw_ops_check(const pf_ops_t *ops)
{
    if (ops == NULL) {
        return CM_ERR_INVALID;
    }
    if (ops->version != PF_OPS_VERSION) {
        return CM_ERR_VERSION;
    }

    /* 九项一个都不能少：允许"可选成员"会让框架里长出永远得不到测试的分支，
     * 而这块板到底能不能用，应该在 `fw_init()` 这一行就答完。 */
    if (ops->caps == NULL || ops->mem_alloc == NULL || ops->mem_free == NULL ||
        ops->clock_ms == NULL || ops->sleep_ms == NULL || ops->mutex_create == NULL ||
        ops->mutex_take == NULL || ops->mutex_give == NULL || ops->task_create == NULL ||
        ops->task_exit_current == NULL) {
        return CM_ERR_INVALID;
    }

    /* 保留位必须读 0（9..31 留给以后的板；写 1 表示"这块板在未来跑"，
     * 框架解释不了，宁可拒绝启动）。 */
    if ((ops->caps() & PF_CAP_RESERVED_MASK) != 0u) {
        return CM_ERR_INVALID;
    }

    return CM_OK;
}
