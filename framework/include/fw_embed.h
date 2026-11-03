/* framework/include/fw_embed.h —— 把框架嵌进别人的固件（阶段 9 的真机缺口）。
 *
 * 为什么需要第二个启动函数：`app_main()` 是**整机固件**的入口（决定 32），它自己
 * 持有 `pf_ops_t` 并调 `pf_platform_ops()`。但产品仓库有自己的 `app_main()`，
 * 它既看不到平台表、也不该看到（A1：App 的编译单元里不存在平台表；产品层同样
 * 不该为"启动框架"去 include `platform/`）。
 *
 * 所以这里只暴露**一个不需要平台表的启动点**：绑表、合并清单、拉起系统 App
 * 全在框架内部完成，调用方只交一份清单。
 *
 * 与 `app_main()` 的分工：
 *   - 整机固件（本仓库的样板固件）走 `app_main()`：启动失败**停在原地**等看门狗，
 *     因为那时没有"上层"可以报告给谁。
 *   - 别人嵌入时走 `fw_embed_start()`：失败**返回码**，停不停由宿主决定——宿主
 *     可能还要继续跑自己的业务，把它拖死是不对的。
 */
#ifndef ESP_AI_SKILL_FW_EMBED_H
#define ESP_AI_SKILL_FW_EMBED_H

#include "common/include/cm_types.h"
#include "framework/include/fw_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动框架：绑定平台表 → 合并清单 → 拉起 `APP_FLAG_SYSTEM` 的 App。
 *
 * @param man 要注册的清单；不得为 NULL。宿主自己决定注册哪些 App，框架**不**
 *            偷偷回落到 `app_manifest_primary()`——"注册了谁"必须是显式的，
 *            隐式回落会让同一份固件在不同链接组合下启动不同的东西。
 * @return `CM_OK` 成功；
 *         `CM_ERR_INVALID` `man` 为 NULL 或其中某条目非法（缺字段、id 越界、
 *                          `stack_bytes < APP_STACK_MIN`、flags 含未知位）；
 *         `CM_ERR_VERSION` 平台表版本与 `PF_OPS_VERSION` 不符（启动期就该拒）；
 *         `CM_ERR_STATE` 已经启动过，或平台表没绑上；
 *         `CM_ERR_NOMEM` 注册表容量不足。
 *         线程与中断：**只在宿主的主任务里调一次**（通常是它的 `app_main()`）；
 *         各 App 在各自任务里跑起来后本函数返回；ISR 禁止。
 *         注意事项：
 *         - 只能**成功**调一次；成功之后的二次调用返回 `CM_ERR_STATE`。
 *           启动失败则把框架状态整体清回未启动（注册表回滚、平台表解绑），
 *           宿主编好参数后可以重试——"失败也烧掉唯一一次机会"没有道理。
 *         - 本函数**不**建宿主的主任务、**不**接管宿主已有的任务；它只负责把
 *           App 任务拉起来，然后返回。
 *         - 整机固件不要调用它，走 `app_main()`：两条启动路径同时存在，会让
 *           "到底谁启动了框架"变成运行时才看得出来的事。
 */
cm_err_t fw_embed_start(const fw_manifest_t *man);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_FW_EMBED_H */
