/* api/af_api/app.h —— App 的入口头（App 只 include 这一个就能写完整）。
 *
 * 它做两件事，**只做两件事**：
 *   1. include `framework/include/fw_app.h`，把 `app_desc_t`、三个回调类型、
 *      `fw_delay_block` / `fw_alloc` / `fw_free` / `fw_log` / `fw_logp` 带进来；
 *   2. 定义 `struct app_ctx` 里 **App 看得见的那几个成员**。
 *
 * 为什么回调签名不在这里重写一遍：同一事实两个家，早晚漂移（D11）。
 * 所以这里一条函数声明都没有——有的全是 `fw_app.h` 的。
 */
#ifndef ESP_AI_SKILL_AF_API_APP_H
#define ESP_AI_SKILL_AF_API_APP_H

#include "framework/include/fw_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一个 App 能登记的资源槽位数。v1 上限，要更多 = 改契约（先过评审）。
 *  槽位由框架在退出时统一回收，App 只负责把地址交给 `res_track()`
 *  （阶段 6 才有 `res_track`，在那之前这里只是布局预留）。 */
#define APP_CTX_SLOTS 8u

/** App 上下文。框架分配、框架填，**App 只读**。
 *
 * 三个区的规矩：
 *   - `id`：本 App 的 id（2..254），只读。**不要再加 `app_id()` 函数**——
 *     字段已经是唯一出处，加函数就是同一事实两个家。
 *   - `slots[]`：资源槽位。App 把 `&ctx->slots[i]` 交给 `res_track()`，
 *     由框架写入真实句柄、由框架在退出时回收；**App 不要自己往里面写**。
 *   - `fw_private[]`：框架自留地（状态、取消位、任务句柄、注册表指针）。
 *     App 读它是"看得到但不保证含义"，写它是未定义行为。
 *
 * 为什么不用"完全隐藏 + 侧表"：`ctx` 是框架分配、App 持有的，App 必须能拿到
 * 槽位地址；藏起来只会逼出一个全局查表函数，那才是真的耦合。所以选择
 * **布局可见、写入权分明**。
 */
struct app_ctx {
    uint32_t id;                      /**< 只读：本 App 的 id */
    void *slots[APP_CTX_SLOTS];       /**< 框架写、框架回收；App 只取地址 */
    uintptr_t fw_private[4];          /**< 框架自留；App 不碰 */
};

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_APP_H */
