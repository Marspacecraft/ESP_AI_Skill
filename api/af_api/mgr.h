#ifndef ESP_AI_SKILL_AF_API_MGR_H
#define ESP_AI_SKILL_AF_API_MGR_H
/* api/af_api/mgr.h —— 管理器面（控制面）
 *
 * 什么时候用这个头：**你要拉起、观察、停下来、回收别的 App**。这是"管理器"
 * （通常是 id 1 的系统 App）的作业面；普通业务 App 不需要它，只需要 `app.h`。
 *
 * 与 `app.h` 的分工（判据：能不能强制别人）：
 *   - `app.h`     使用面 —— 我只想干自己的活（延时、分配、日志、心跳）。
 *   - `mgr.h`     控制面 —— 我要让别人开始跑 / 停下来 / 被回收。
 * 两件事的失败模式完全不同：前者错了只影响自己，后者错了会让别人的任务停在
 * 半个状态里，所以它们不混在同一个头里。
 *
 * 谁负责什么（这是本框架最重要的一条分工，别越界）：
 *   - 框架：`fw_boot()` 只拉起 `APP_FLAG_SYSTEM` 的 App；取消位命中时走完
 *           退出五步（清位 → exit() 恰好一次 → 回收 → 终态 → 结束任务）。
 *   - 管理器：决定"还要不要起别的 App"、起哪些、什么时候停、停了之后什么时候
 *           摘表并释放上下文（见 `fw_live_remove()` 的注意事项）。
 *   - App：只需要 `for (;;) { 干活; fw_delay_block(ms); }`，不许自己管状态机。
 *
 * 线程与中断：本头里除 `fw_app_request_cancel()` 之外都不是 ISR 安全的；
 * 全部应当从管理器自己的任务里调用。
 */

#include "api/af_api/app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 实例状态 ───────────────────────────────────────────────────── */

/** 实例的生命周期状态。没有"暂停"——暂停是可以不实现的，停下来是可实现的。 */
typedef enum {
    FW_APP_STATE_EMPTY = 0,    /**< 上下文已分配，任务还没建。 */
    FW_APP_STATE_INIT = 1,     /**< 任务已建，`init()` 正在跑（尚未返回）。 */
    FW_APP_STATE_RUNNING = 2,  /**< `init()` 成功返回，`task()` 在跑。 */
    FW_APP_STATE_STOPPING = 3, /**< 已进入退出路径，`exit()` 可能正在跑。 */
    FW_APP_STATE_STOPPED = 4, /**< 正常退出：取消位命中，`exit()` 恰好调过一次。 */
    FW_APP_STATE_FAILED = 5,  /**< 失败退出：`init()` 非 CM_OK，或 `task()` 返回。 */
} fw_app_state_e;

/** 不透明的实例上下文。App 只能持有指针，**不许解引用**（布局属于框架内部）。 */
typedef struct fw_app_ctx fw_app_ctx_t;

/* ── 选一个 id ──────────────────────────────────────────────────── */

/** 领一个**从未用过**的 id（D18：重启不复用旧 id）。
 *
 * @param out 输出参数；不得为 NULL。
 * @return `CM_OK` 已写入 `[2,254]` 内最小的空闲 id 并标记为已用；
 *         `CM_ERR_NOMEM` 号码发完了（253 个全用光）。失败时 `*out` 不变。
 *         线程与中断：仅管理器在"要新建实例"时调用；ISR 禁止。
 *         注意事项：**没有对应的释放函数**——id 只增不减。复用旧 id 会让拿着它
 *                   的人静默操作到新实例上，代价远大于号码耗尽。
 *                   `fw_init()` 成功时，清单里出现过的 id 已经算被占。
 */
cm_err_t fw_alloc_id(uint32_t *out);

/* ── 造实例 / 启动 / 停止 ───────────────────────────────────────── */

/** 分配一个上下文并做静态初始化，id 取 `desc->id`（首次启动用这个）。
 *
 * @param desc 注册表里那份描述体；不得为 NULL，`desc->id` 已在 `fw_init()` 校验过。
 * @return 上下文指针；平台表未绑或内存不足返回 NULL。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：由框架的分配器给出，必须 `fw_app_ctx_free()` 释放；
 *                   失败时不留任何状态。新实例的 id 必须与旧实例不同。
 */
fw_app_ctx_t *fw_app_ctx_new(const app_desc_t *desc);

/** 同 `fw_app_ctx_new()`，但**显式指定实例 id**（重启用这个，D18）。
 *
 * @param desc 描述体；不得为 NULL。
 * @param id   实例 id，必须 ∈ `[2,254]`，且必须是 `fw_alloc_id()` 发出来的。
 * @return 上下文指针；参数非法、平台表未绑或内存不足返回 NULL。
 *         线程与中断：仅管理器侧；ISR 禁止。
 *         注意事项：**不会**检查 id 是否已被别人用掉——那是 `fw_alloc_id()` 的
 *                   职责；这里只保证范围合法。
 */
fw_app_ctx_t *fw_app_ctx_new_with_id(const app_desc_t *desc, uint32_t id);

/** 释放上下文。**只能在任务已经结束之后调**（任务还在跑就释放 = UAF）。
 *
 * @param c `fw_app_ctx_new*()` 的产物，可为 NULL（无操作）。
 *         线程与中断：管理器任务里；ISR 禁止。
 *         注意事项：**释放会自动把它从活实例表里摘掉**，所以漏写 `fw_live_remove()`
 *                   也不会留下悬垂指针。想"先认领、再释放"两步走仍然可以：显式
 *                   `fw_live_remove()` 之后再调这里（对不在表里的指针无操作）。
 */
void fw_app_ctx_free(fw_app_ctx_t *c);

/** 为上下文创建任务，任务里依次跑 `init()` → `task()`。
 *
 * @param c `fw_app_ctx_new*()` 的产物。
 * @return `CM_OK` 任务已创建；`CM_ERR_NOMEM` 平台拒绝建任务；`CM_ERR_STATE`
 *         `c` 为 NULL 或已经启动过。
 *         线程与中断：管理器任务里；ISR 禁止。
 *         注意事项：任务入口会把状态推成 `INIT` → `RUNNING`；`init()` 失败或
 *                   `task()` 返回都会走退出路径，那条路径**不返回**，所以本函数
 *                   返回后状态还会继续变——要等 `RUNNING` 得自己轮询 `fw_app_state()`。
 */
cm_err_t fw_app_start(fw_app_ctx_t *c);

/** 置取消位：让这个实例在**下一个让出点**走退出路径。
 *
 * @param c 目标实例；NULL 时无操作。
 *         线程与中断：可以跨任务；ISR 里能调（只写一个整数），但不在 v1 承诺范围内。
 *         注意事项：它只是"请求"，不是"立刻停"。目标可能正卡在一段不含让出点的
 *                   纯计算里——那种情况框架既发现不了也停不掉（设计边界）。
 *                   已经处于退出路径（`exit()` 正在跑）时再置位不会二次退出。
 */
void fw_app_request_cancel(fw_app_ctx_t *c);

/* ── 观察 ───────────────────────────────────────────────────────── */

/** 这个实例的 id（`[2,254]`）。`c` 为 NULL 时返回 `APP_ID_INVALID`。
 *
 * 为什么需要它：`fw_app_ctx_t` 是不透明的，管理器看得见指针却读不到 id，
 * 而日志、服务归属、去重判断全都要 id。只暴露这一个读值函数，不暴露布局。
 * 注意事项：id 在实例生命周期内不变；重启是**新实例、新 id**（D18）。 */
uint32_t fw_app_id(const fw_app_ctx_t *c);

/** 当前状态。`c` 为 NULL 时返回 `FW_APP_STATE_EMPTY`。
 *  注意事项：这是某一瞬间的快照，下一秒可能已经变了。 */
fw_app_state_e fw_app_state(const fw_app_ctx_t *c);

/** `exit()` 至今被调用过几次。正常是 1（含启动失败）。
 *  @param c 目标实例；NULL 时返回 0。 */
uint32_t fw_app_exit_calls(const fw_app_ctx_t *c);

/** **所有 `fw_app_start()` 拉起、且尚未被摘表**的实例个数。
 *  包含已经进 STOPPED/FAILED 的——它们在管理器回收之前一直留在表里（D53），
 *  这样"它停了/它崩了"才是可观测的。 */
uint32_t fw_live_count(void);

/** 第 `index` 个活实例的上下文（按启动顺序，跳过已摘表的空洞）；越界返回 NULL。
 *
 * @param index 从 0 开始。
 *         注意事项：这是某一瞬间的快照，别的任务摘表会让后面的项前移；
 *                   遍历时以 `fw_live_count()` 为准，别缓存下标。
 */
fw_app_ctx_t *fw_live_at(uint32_t index);

/** 把一个实例从活实例表里摘掉（**不释放它**，见 D19）。
 *
 * @param c  `fw_live_at()` 拿到的指针；不在表里时无操作。
 *         注意事项：**先 `fw_live_remove()` 再 `fw_app_ctx_free()`**——顺序反了
 *                   就会在表里留下一根悬垂指针，`fw_live_at()` 之后读到的是野内存。
 *                   只能对已经进 STOPPED/FAILED 的实例调用；还在跑就摘表，等于
 *                   把"谁负责停它"这件事弄丢了。
 *                   摘表是"我认领了它的结局"这一步——**没摘表的死实例会一直留着**，
 *                   所以管理器漏掉一个，表里就永远多一个（不会被框架偷偷清掉）。
 */
void fw_live_remove(fw_app_ctx_t *c);

/* ── 清单里有什么（管理器要知道"可以起谁"）────────────────────── */

/** `fw_init()` 合并进来的描述体个数（等于所有清单条目之和）。 */
uint32_t fw_app_count(void);

/** 第 `index` 个描述体；越界返回 NULL。
 *  注意事项：描述体是静态常量，指向它们没有所有权问题；但"清单里有"不等于
 *            "已经起来"——那要看 `fw_live_count()`。 */
const app_desc_t *fw_app_at(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_MGR_H */
