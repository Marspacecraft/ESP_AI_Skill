/* api/af_api/res.h —— 资源与配额（App 可见的契约，D44）。
 *
 * 归类依据：这两件事都是"App 向框架要东西、并受人管"的接口——内存句柄走槽位，
 * 用量走配额。实现在 `framework/src/fw_res.c` / `fw_quota.c`，但那不是读它的理由：
 * **App 只需要这个头。**
 *
 * 依赖约束（`tools/check_link.py` 会验）：本头只能 include `common/`，
 * 不得 include `framework/` 或 `platform/`——否则 App 的编译单元就顺着它摸进实现层。
 */
#ifndef ESP_AI_SKILL_AF_API_RES_H
#define ESP_AI_SKILL_AF_API_RES_H

#include "common/include/cm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct app_ctx; /* 前向声明：本头只用指针，不碰布局 */

/* ── 资源（步骤 6.1）────────────────────────────────────────────
 *
 * 一条规矩：**App 不持有裸句柄**。拿到的内存/外设只以 `ctx->slots[i]` 的形式
 * 存在，框架在那条路径上负责还——这样"忘了释放"不是 App 的纪律，而是结构上
 * 没机会发生。
 *
 * 为什么不是"先获取、再登记到槽位"（`res_track(slot, handle)` 那种写法）：
 * C 的参数求值顺序是未指定的，而获取函数一旦在内部命中取消位就**不返回**
 * （`fw_run_exit()`），于是登记那一步永远不会执行，句柄已经泄漏。要让它成立，
 * 就得给每个获取函数加一条"成功之后不得再查取消位"的默契——每个接口各自守，
 * 漏一个漏一个。改成获取函数**自己写槽位**之后，这条默契不再需要（D25，
 * 被否决的写法留在 §7 决策表里）。
 */

/** 资源种类。v1 只有堆内存一种；外设资源等阶段 7/9 有真实需求再加值。 */
typedef enum {
    RES_KIND_MEM = 0, /**< 堆内存：内部就是 `ops->mem_alloc(bytes)` */
} res_kind_e;

/** 资源活到什么时候。**决定退出路径管不管它**，不是优先级也不是大小。 */
typedef enum {
    RES_LIFE_APP = 0,  /**< 我拿的，我走了请帮我收（绝大多数情况的默认值） */
    RES_LIFE_BOOT = 1, /**< 借来的，不属于我，**别替我收**（整机共享外设） */
} res_life_e;

/** 申请一个资源并登记进槽位。
 *
 * @param slot  必须是 `&ctx->slots[0..APP_CTX_SLOTS-1]` 之一；不得为 NULL。
 * @param kind  资源种类，见 `res_kind_e`。
 * @param bytes 字节数；**必须 > 0**（0 字节请求按参数错误处理，不返回空指针）。
 * @param life  生命周期，见 `res_life_e`；省略语义时传 `RES_LIFE_APP`。
 * @return `CM_OK` 槽位已写入句柄；`CM_ERR_INVALID` 参数非法或槽位不属于本 App；
 *         `CM_ERR_BUSY` 槽位里已经有东西（**不覆盖，先释放再来**）；
 *         `CM_ERR_NOMEM` 平台分配失败（槽位保持空着，不留半个登记）；
 *         `CM_ERR_STATE` 不在 App 任务里调（没有"当前上下文"就无处登记）。
 *         线程与中断：App 任务里调（`init()`/`task()` 里都行）；ISR 禁止。
 *         注意事项：失败时槽位**一定**还是空的——调用方不需要回滚。
 */
cm_err_t res_acquire(void *slot, res_kind_e kind, size_t bytes, res_life_e life);

/** 提前释放一个资源（不想等到退出时）。
 *
 * @param slot 槽位地址；槽位为空或不属于本 App 时**无操作**（重复调用安全）。
 * @return 无。
 *         线程与中断：App 任务里调；ISR 禁止。
 *         注意事项：释放后 `ctx->slots[i]` 立刻变回 NULL，可以再 acquire。
 */
void res_release(void *slot);

/* ── 配额（控制面，管理器专用，步骤 6.2）────────────────────────
 *
 * 配额限制的是**一个 App 自己**能用多少堆内存。它不是安全边界（越界的 App
 * 仍然能调 `fw_alloc`），而是一个**故障放大器**：一个漏释放的 App 会在自己的
 * 配额里撞墙，而不是把别人的内存一起吃掉。
 *
 * 与 `CM_ERR_NOMEM` 的分工：`QUOTA` 是「你自己用超了」，`NOMEM` 是「整机没了」。
 * 合成一个码，现场只能靠猜——前者该查漏释放，后者该查是不是真该省着用。
 */

/** 设置某个实例的配额。**只有管理器该调**。
 *
 * @param ctx   目标实例；不得为 NULL。
 * @param bytes 配额字节数；**0 表示不限**（默认）。
 * @return `CM_OK`；`CM_ERR_INVALID` `ctx` 为 NULL。
 *         线程与中断：管理器任务里调；ISR 禁止。非阻塞。
 *         注意事项：**下调到低于当前用量是允许的**——它不会立刻杀谁，只是让
 *                   这个 App 的下一次分配失败（`fw_alloc` 返回 NULL、
 *                   `res_acquire` 返回 `CM_ERR_QUOTA`）。要在下调后立刻生效，
 *                   配套用 `fw_res_revoke()`。
 */
cm_err_t fw_res_set_quota(struct app_ctx *ctx, size_t bytes);

/** 撤销剩余配额：把上限压到**当前已用量**，于是任何新分配都会失败。
 *
 * @param ctx 目标实例；不得为 NULL。
 * @return `CM_OK`；`CM_ERR_INVALID`。
 *         线程与中断：管理器任务里调；ISR 禁止。
 *         注意事项：不停止该 App、不发取消位——"停不停"是管理器的决定，
 *                   不是配额机制的。已经分配的内存不会被动释放（那是 App 的
 *                   清理，归 `exit()`）。
 */
cm_err_t fw_res_revoke(struct app_ctx *ctx);

/** 这个实例当前的用量/配额/被拒次数。诊断与测试用。
 *
 * @param ctx 目标实例；NULL 时全部写 0。
 * @param used_bytes     可为 NULL；当前记账的字节数（只算 `fw_alloc` 与
 *                       `RES_KIND_MEM` 的 `res_acquire`）。
 * @param quota_bytes    可为 NULL；0 表示不限。
 * @param denied_count   可为 NULL；因配额被拒的次数。
 *         线程与中断：任意任务可调；ISR 禁止。
 */
void fw_res_usage(const struct app_ctx *ctx, size_t *used_bytes, size_t *quota_bytes,
                  uint32_t *denied_count);


#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_RES_H */
