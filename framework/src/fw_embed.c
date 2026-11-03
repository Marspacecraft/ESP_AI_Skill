/* framework/src/fw_embed.c —— 给"别人的 app_main"用的启动点（阶段 9 真机缺口）。
 *
 * 它与 `fw_main.c` 的唯一区别是**失败之后怎么办**：`app_main()` 停在原地等看门狗
 * （整机固件没有上层可报告），这里返回错误码（宿主可能还要跑自己的业务）。
 *
 * 顺序与 `fw_startup()` 完全一致，不另写一份：bind → init → boot 这三步的顺序
 * 是契约（先校验平台表，后合并清单），第二份实现迟早漂移。
 */
#include "framework/include/fw_embed.h"

#include "framework/src/fw_internal.h"
#include "platform/port/pf_ops.h"

/* 成功启动过就置位。用原子是因为将来宿主可能从别的任务里调它（虽然契约上
 * 要求只在主任务调一次），而"两个任务同时启动框架"必须有一个拿到 STATE。 */
static uint32_t s_embed_used;

cm_err_t fw_embed_start(const fw_manifest_t *man)
{
    const pf_ops_t *ops;
    cm_err_t err;

    if (man == NULL) {
        return CM_ERR_INVALID;
    }
    if (CM_ATOMIC_EXCHANGE(&s_embed_used, 1u, CM_MEM_ACQ_REL) != 0u) {
        return CM_ERR_STATE;
    }

    ops = pf_platform_ops();
    if (ops == NULL) {
        /* 平台没提供表：这是平台违约，不是"清单写错了"。允许修好后重试。 */
        CM_ATOMIC_STORE(&s_embed_used, 0u, CM_MEM_RELEASE);
        return CM_ERR_STATE;
    }

    err = fw_startup(ops, man);
    if (err != CM_OK) {
        /* 失败要**连平台表的绑定一起退掉**：`fw_startup()` 的回滚只覆盖注册表，
         * 而 `fw_platform_bind()` 一旦成功过就拒绝第二次绑定（那是防止整机
         * 半启动的闸门）。不 `fw_reset()` 的话，宿主"改对参数再试一次"会拿到
         * `CM_ERR_STATE`——它没做错任何事，却再也没有机会。 */
        fw_reset();
        return err;
    }
    return CM_OK;
}

void fw_embed_forget(void)
{
    CM_ATOMIC_STORE(&s_embed_used, 0u, CM_MEM_RELEASE);
}
