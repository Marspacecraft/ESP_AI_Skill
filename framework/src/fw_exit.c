/* framework/src/fw_exit.c —— 退出路径（步骤 2.4 的另一半）。
 *
 * 这个函数是整个框架里唯一"调用后不返回"的用户代码路径。它之所以单独成一个
 * 文件，是因为**顺序本身就是契约**：五步里挪动任何一步都会造出一类永不结束
 * 的 App 或一次也不回收的退出。把它放在单独的文件里，改动时至少会撞到这段注释。
 *
 * 顺序（改顺序 = 改契约）：
 *   ① 先清取消位 —— 否则 `exit()` 里再调阻塞接口，会当场又命中一次取消。
 *   ② `exit()` 恰好一次 —— 用 `exit_calls` 做闸门，不靠调用方自觉。
 *   ③ 摘掉本 owner 注册的服务、按生命周期回收资源槽位。
 *      **服务只在 owner 死后"摘掉持有者"、不移除表项**（D23）：保留表项，
 *      `CM_ERR_UNAVAILABLE`（崩了、可重试）才可达，否则与
 *      `CM_ERR_NOT_SUPPORTED`（这块板没这能力）变成同一个意思。
 *      资源只回收 `RES_LIFE_APP`；`RES_LIFE_BOOT` 是借来的，不动（D25）。
 *   ④ 置终态。
 *   ⑤ 结束当前任务 —— 平台保证"不返回"。
 *
 * 为什么 ② 之前要置 `STOPPING` 而不是直接写终态：`exit()` 跑的时候状态叫
 * "正在停"，不是"已停"；外部（管理器、状态查询）据此能区分"还在清理"与
 * "清理完了"。
 */
#include "framework/src/fw_internal.h"

void fw_run_exit(fw_app_ctx_t *c, fw_app_state_e end_state)
{
    const pf_ops_t *ops;

    if (c == NULL) {
        /* 不可能路径：调用者手上一定有上下文。这里不 assert，是因为本仓
         * 还没有统一的断言出口；用死循环把错误钉在原地，比带着空指针往下走安全。 */
        for (;;) {
        }
    }

    ops = fw_ops();

    /* ① 先清取消位。 */
    CM_ATOMIC_STORE(&c->cancel, 0u,
                    CM_MEM_RELEASE); /* 先清位：否则 exit() 里的阻塞再次命中取消位 */
    CM_ATOMIC_STORE(&c->in_exit, 1u, CM_MEM_RELEASE); /* 清理期间取消无效 */

    /* ② exit() 恰好一次。 */
    if (CM_ATOMIC_EXCHANGE(&c->exit_calls, 1u, CM_MEM_ACQ_REL) == 0u) {
        CM_ATOMIC_STORE(&c->state, (uint32_t)FW_APP_STATE_STOPPING, CM_MEM_RELEASE);
        c->desc->exit(&c->pub);
    }

    /* ③ 摘服务 + 回收资源。
     *    服务先摘：它引用的是上下文本身，若等到上下文被管理器释放再摘，
     *    查表的一方会拿到一根野指针。 */
    fw_svc_drop_owner(c);
    /* 只回收 RES_LIFE_APP 的槽位；BOOT 的是借来的，不动（见 fw_app.h）。 */
    fw_res_release_all(c);

    /* ④ 终态。 */
    CM_ATOMIC_STORE(&c->state, (uint32_t)end_state, CM_MEM_RELEASE);

    /* ⑤ 结束当前任务，之后一行都不会执行。 */
    if (ops != NULL && ops->task_exit_current != NULL) {
        ops->task_exit_current();
    }

    /* 平台违约（`task_exit_current` 为 NULL）时停在这里：任务必须结束，
     * 不能带着 STOPPED/FAILED 的状态继续跑。 */
    for (;;) {
    }
}
