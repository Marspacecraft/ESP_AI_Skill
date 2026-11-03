/* framework/src/fw_main.c —— 整机唯一入口（决定 32，步骤 4.1）。
 *
 * 调用链只有一条，不许有第二条：
 *
 *     app_main()  →  pf_platform_ops()    （平台把表交出来）
 *                 →  fw_platform_bind()   （校验并绑定）
 *                 →  fw_init()            （合并清单、查重、校验每一条）
 *                 →  fw_boot()            （只拉起 APP_FLAG_SYSTEM 的）
 *
 * 这一份文件里**没有** `pf_` 前缀的实现，也没有 App 的名字：平台是谁、有哪些 App，
 * 都由两个钩子回答（`pf_platform_ops()` 由平台实现，`app_manifest_primary()` 由
 * App 层实现）。所以加一块板、加一个 App 都不需要改这里——它只负责"顺序"。
 *
 * 为什么把启动拆成 `fw_startup()` + `app_main()`：
 *   `app_main()` 里有 `for (;;)`（启动失败要停在原地等看门狗），一调就下不来。
 *   测试要验的是"启动顺序对不对"，不是"能不能永久阻塞"，所以把可测的那段抽出来，
 *   剩下三行的阻塞循环没有测试价值。
 */
#include "framework/src/fw_internal.h"
#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

/** 启动失败时停在原地的心跳间隔。 */
#define FW_HALT_HEARTBEAT_MS 1000u

cm_err_t fw_startup(const pf_ops_t *ops, const fw_manifest_t *man)
{
    cm_err_t err;

    if (ops == NULL || man == NULL) {
        return CM_ERR_INVALID;
    }

    /* 顺序不能换：bind 先校验版本与九项完整性，后面的 init/boot 都假设表可用。 */
    err = fw_platform_bind(ops);
    if (err != CM_OK) {
        return err;
    }
    err = fw_init(man, 1u);
    if (err != CM_OK) {
        return err;
    }
    return fw_boot();
}

void app_main(void)
{
    cm_err_t err;
    const pf_ops_t *ops = pf_platform_ops();

    /* ops 为 NULL 时连 log 都用不了（日志走 ops），只能直接停：这属于平台违约，
     * 在真机上表现为"上电没动静"，而那正是它该有的表现。 */
    if (ops == NULL) {
        for (;;) {
            /* 没有任何可用的平台接口，连让出 CPU 都做不到；此处只能是空转。 */
        }
    }

    err = fw_startup(ops, app_manifest_primary());
    if (err != CM_OK) {
        fw_log("启动失败：%s（停在原地，等看门狗复位）", cm_err_str(err));
        /* 不返回：返回等于把"启动失败"伪装成"主任务正常结束"。阶段 8 的看门狗
         * 会把"停在这里"变成复位；在此之前，停住比假装活着更好查。 */
        for (;;) {
            ops->sleep_ms(FW_HALT_HEARTBEAT_MS);
        }
    }

    /* 启动成功：框架自己的活干完了。各 App 跑在各自的平台上，主任务退出——
     * 用平台的 `task_exit_current()` 而不是 `return`，因为 FreeRTOS 上
     * "主任务 return" 与 "删除自己" 不是同一件事。 */
    ops->task_exit_current();
}
