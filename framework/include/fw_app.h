/* framework/include/fw_app.h —— App 描述体与框架→App 的使用面。
 *
 * 这个头是"框架给 App 的词汇表"：App 长什么样（`app_desc_t`）、怎么被登记
 * （`fw_manifest_t`）、能调哪些东西（`fw_delay_block` / `fw_alloc` / `fw_free` /
 * `fw_log` / `fw_logp`）。
 *
 * 它与 `api/af_api/app.h` 的分工（D11，2026-11-03 由 D17 修订）：
 *   - **本头是唯一出处**：回调签名、`app_desc_t`、`fw_manifest_t`、使用面函数、
 *     `struct app_ctx` 的布局，全都在这里；
 *   - `api/af_api/app.h` 只做一件事：include 本头。
 *   为什么 `app_ctx` 也搬进来：框架要实现它（要往里放状态、取消位、任务句柄），
 *   而框架不能反向 include `api/`——那是 App 的包。一个只有 App 能看见、框架
 *   看不见的结构体，框架是没法实现的（D17）。
 *
 * 硬约束（读权不传递的第一次实战）：
 *   **本头只准 include `common/include/`。** 不许 include `platform/port/`——
 *   否则框架的编译单元就间接拿到了平台表，App 也能顺着 include 链摸过去。
 *   验证方式：`clang -M` 算出闭包，必须 ⊆ {common/include, 本头自身}。
 */
#ifndef ESP_AI_SKILL_FW_APP_H
#define ESP_AI_SKILL_FW_APP_H

#include "common/include/cm_types.h"

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
 *     字段已经是唯一出处，加函数就是同一事实两个家（D13）。
 *   - `slots[]`：资源槽位。App 把 `&ctx->slots[i]` 交给 `res_acquire()`，
 *     由框架写入真实句柄、由框架在退出时回收；**App 不要自己往里面写，也不要
 *     自己释放**（要提前还就用 `res_release(&ctx->slots[i])`）。
 *   - `fw`：框架自留地（指向框架内部上下文的不透明指针）。App 读或写它
 *     都是未定义行为；它存在只是因为框架需要从 App 手里的这个地址回到
 *     自己的状态（D16）。
 *
 * 为什么不用"完全隐藏 + 侧表"：`ctx` 是框架分配、App 持有的，App 必须能拿到
 * 槽位地址；藏起来只会逼出一个全局查表函数，那才是真的耦合。所以选择
 * **布局可见、写入权分明**。
 */
struct app_ctx {
    uint32_t id;                      /**< 只读：本 App 的 id */
    void *slots[APP_CTX_SLOTS];       /**< 框架写、框架回收；App 只取地址 */
    void *fw;                         /**< 框架自留；App 不碰（D16） */
};

typedef struct app_ctx app_ctx_t;

/* ── 三个回调 ──────────────────────────────────────────────────── */

/** 初始化。返回 CM_OK 才进 RUNNING；返回任何非 CM_OK 都进 FAILED。 */
typedef cm_err_t (*app_init_fn)(app_ctx_t *ctx);

/** 退出清理。**在任何结束路径上恰好调用一次**（含 FAILED）。
 *  这里可以调阻塞接口（框架已先清掉取消位），但别再写清理之外的事。 */
typedef void (*app_exit_fn)(app_ctx_t *ctx);

/** 主任务。强制形态 `for(;;){ do_work(); fw_delay_block(N); }`。
 *  返回即表示"这个 App 自己认为自己挂了"→ FAILED。 */
typedef void (*app_task_fn)(app_ctx_t *ctx);

/* ── 描述体与清单 ──────────────────────────────────────────────── */

/** 系统 App：`fw_boot()` 先拉起带这个标志的 App，再由它们拉起业务 App。
 *  它不是特权等级，只决定启动先后。 */
#define APP_FLAG_SYSTEM (1u << 0)

/** App 的 id 取值域。0 = 无效哨兵，1 = 管理器，2..254 给普通 App，255 禁用。 */
#define APP_ID_INVALID  0u
#define APP_ID_MANAGER  1u
#define APP_ID_MIN      2u
#define APP_ID_MAX      254u

/** App 主任务的最小栈（字节）。**必须与 `PF_TASK_MIN_STACK` 一致**——
 *  框架先用本值挡一道（启动前就报错），平台再用自己的值挡一道（`task_create`
 *  返回 NULL）。两处数字不一致时以平台为准；身份 5 的补齐清单里有一条是把它们
 *  用静态断言钉在一起（`§6.7`）。
 *  为什么不让 App 自己随便写小：栈溢出的症状是随机的，启动时就拒绝比运行期
 *  踩坏内存好定位。 */
#define APP_STACK_MIN  1024u

/** 一个 App 的全部声明。**由清单持有，必须是静态常量**（框架不拷贝名字）。 */
typedef struct app_desc {
    uint32_t id;             /**< 2..254；0/1 保留；重复 = 启动失败 */
    const char *name;        /**< 只用于日志；不得为 NULL；不参与任何判定 */
    app_init_fn init;        /**< 必需；NULL 视为清单错误 */
    app_exit_fn exit;        /**< 必需；NULL 视为清单错误（否则资源无人回收） */
    app_task_fn task;        /**< 必需；NULL 视为清单错误 */
    uint32_t flags;          /**< `APP_FLAG_SYSTEM` 等位；未知位必须为 0 */
    uint32_t stack_bytes;    /**< 主任务栈，>= `APP_STACK_MIN`；**平台按此分配，不许自动抬栈** */
} app_desc_t;

/** 一份清单。**count 与指针必须自洽**：不靠 NULL 结尾（NULL 是合法的描述体指针，
 *  用 NULL 结尾会让"重复 id"检查漏掉最后一个）。 */
typedef struct fw_manifest {
    const app_desc_t *const *apps; /**< 指向静态数组；count>0 时不得为 NULL */
    uint32_t count;                /**< 0 表示这份清单为空（合法，但不算启动成功） */
} fw_manifest_t;

/** 本仓库自带的 App 清单。**由 App 层实现**（`apps/app_manifest.c`）。
 *
 * @return 指向静态清单的指针，**不得为 NULL**；`count == 0` 视为"没有任何 App"，
 *         `fw_init()` 会以 `CM_ERR_INVALID` 拒绝启动。
 *         线程与中断：只在启动阶段调用一次；实现必须是"返回常量地址"这种无副作用形态
 *                     （`app_main()` 之前不允许分配内存——那时 `fw_alloc` 还没绑表）。
 *         注意事项：框架不知道自己链接了哪些 App，**只能问这个函数**。加 App 就是
 *                   改 `apps/app_manifest.c` 一处，不用动框架。
 */
const fw_manifest_t *app_manifest_primary(void);

/* ── 资源（步骤 6.1）────────────────────────────────────────────
 *
 * 一条规矩：**App 不持有裸句柄**。拿到的内存/外设只以 `ctx->slots[i]` 的形式
 * 存在，框架在那条路径上负责还——这样"忘了释放"不是 App 的纪律，而是结构上
 * 没机会发生。
 *
 * 为什么不是"先获取、再 `res_track(slot, handle)` 登记"：C 的参数求值顺序是
 * 未指定的，而获取函数一旦在内部命中取消位就**不返回**（`fw_run_exit()`），
 * 于是 `res_track` 永远不会执行，句柄已经泄漏。要让它成立，就得给每个获取
 * 函数加一条"成功之后不得再查取消位"的默契——每个接口各自守，漏一个漏一个。
 * 改成获取函数**自己写槽位**之后，这条默契不再需要。
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

/* ── 使用面（App 能调的全部框架函数）───────────────────────────── */

/** 伪阻塞延时：等至少 ms 毫秒，**返回之后的行可能永远不执行**。
 *
 * @param ms 毫秒；0 表示只让出一次 CPU。框架按 ≤20ms 切片等待，每片查一次取消位。
 * @return 无。
 *         线程与中断：只能在 App 任务里调用；**ISR 里禁止**。
 *         注意事项：命中取消位时框架直接走退出路径（清取消位 → `exit()` →
 *                   回收资源 → 结束任务），**不返回**。所以清理逻辑只能写在
 *                   `exit()` 里，不能写在这行下面。
 */
void fw_delay_block(uint32_t ms);

/** 从框架的内存池分配。
 *
 * @param bytes >0；0 返回 NULL。
 * @return 至少 bytes 字节；失败 NULL。
 *         线程与中断：任意任务可调；ISR 里禁止。
 *         注意事项：**优先用静态内存**，动态分配要评审（谁释放、何时释放）。
 *                   不要用 malloc/free 家族，也不要 new/delete——它们在本仓被禁。
 */
void *fw_alloc(size_t bytes);

/** 归还 `fw_alloc` 的指针；NULL 无操作。
 *  线程与中断：任意任务可调；ISR 里禁止。
 *  注意事项：退出路径上框架会回收 `res_track` 登记的槽位；这里只处理自己拿的。 */
void fw_free(void *p);

/** 打一行日志（自动带级别与 App id 前缀）。
 *
 * @param fmt printf 风格格式串，不得为 NULL。
 * @return 无。格式化失败不崩溃（截断到固定长度）。
 *         线程与中断：任意任务可调；**ISR 里用 `fw_log` 是不安全的**（会阻塞在串口上）。
 *         注意事项：不要在日志里拼长字符串再传入，直接格式化。
 */
void fw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** 带标签的日志（标签用于区分模块，如 "stor" / "disp"）。
 *
 * @param tag 短标签，不得为 NULL；超过 15 字节截断。
 * @param fmt 同上。
 * @return 无。
 *         线程与中断：同 `fw_log`。
 *         注意事项：tag 必须是静态字符串，框架不拷贝它。
 */
void fw_logp(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_FW_APP_H */
