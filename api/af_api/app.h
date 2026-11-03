/* api/af_api/app.h —— **App 唯一需要 include 的头**（D44）。
 *
 * 它做三件事：定义 App 的形状（`app_desc_t` / 回调 / 上下文）、给出使用面
 * （`fw_delay_block` / `fw_alloc` / `fw_free` / `fw_log` / `fw_logp` / 心跳）、
 * 并把另外三份契约（资源 `res.h`、事件 `evt.h`、服务 `svc.h`）带进来。
 *
 * 历史（别再倒回去）：
 *   - D11：这些声明曾经住在 `framework/include/fw_app.h`；
 *   - D17：为了"框架要写 `app_ctx`、而框架 include 不了 `api/`"，整体搬去 framework；
 *   - **D44 推翻 D17 的前提**——`link 1 2 3 api/af_api/ framework/include/` 允许
 *     tag 3 读 `api/af_api/`，所以"框架 include 不了 api/"根本不成立。契约回到契约该在的
 *     地方：`api/af_api/` 是唯一出处，`framework/include/fw_app.h` 反过来 include 它。
 *
 * 依赖约束（`tools/check_link.py` 会验）：闭包必须 ⊆ {`api/af_api/`, `common/`}。
 * 本头**不得** include `framework/` 或 `platform/`——否则"App 不必读实现"这句话
 * 就变成"App 读得到实现"。
 */
#ifndef ESP_AI_SKILL_AF_API_APP_H
#define ESP_AI_SKILL_AF_API_APP_H

#include "common/include/cm_types.h"
#include "api/af_api/res.h"
#include "api/af_api/evt.h"
#include "api/af_api/svc.h"

#ifdef __cplusplus
extern "C" {
#endif
/** 一个 App 能登记的资源槽位数。v1 上限，要更多 = 改契约（先过评审）。
 *  槽位由 `res_acquire()` 直接写入、由框架在退出时统一回收（D25）；
 *  没有"先获取再登记"那一步——那一步的漏洞见下方资源一节。 */
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
 *  踩坏内存好定位。
 *
 *  为什么是 3072 而不是 1024（实测，ESP32-C3 + ESP-IDF 5.5.3）：
 *  在一张 1024 字节的栈上只调一次 `fw_logp()`（4 个参数 + 120 字节字符串），
 *  设备直接 `Guru Meditation Error: ... (Stack protection fault)` 复位；
 *  同样的调用在 2048 字节栈上安全返回。`fw_vlog()` 自己占 256 字节缓冲，
 *  再叠 newlib 的 `vsnprintf()`，1 KB 连"能打一行日志"都做不到。
 *  2048 是实测下限，取 3072 留一档余量——App 的栈里除了日志还要放它自己的
 *  局部变量。 */
#define APP_STACK_MIN  3072u

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
 *  注意事项：这里只还自己拿的指针；`ctx->slots[]` 里的资源由框架在退出路径
 *           上按 `res_life_e` 回收（`RES_LIFE_APP` 收、`RES_LIFE_BOOT` 不收），
 *           不要在这里手动重复还。 */
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

/* ── 心跳（步骤 8.1 / 8.2）──────────────────────────────────────
 *
 * 心跳不是"我活着"的声明，而是"我又走完了一段可取消的等待"的事实。它由框架在
 * 让出点自动喂：`fw_delay_block()`、`fw_event_wait()`、`fw_svc_submit()`。
 *
 * **它证明不了"还在干活"**：一个纯死循环不调任何框架接口，心跳就永远不动——
 * 那正是要发现的那种 App。所以这里只提供**观测**，动手不在这里：
 * 本仓库负责"能被停下来"，产品仓库负责"发现停不下来时重启整机"。
 */

/** 读一个实例的心跳。
 *
 * @param ctx     目标实例；NULL 时全部写 0。
 * @param seq     可为 NULL；心跳序号（单调增，回绕是 49.7 天量级）。
 * @param age_ms  可为 NULL；距上一次喂心跳过了多久。
 * @return 无。
 *         线程与中断：任意任务可调；ISR 禁止。
 *         注意事项：`age_ms` 只在实例处于 RUNNING 时有意义；EMPTY/INIT 阶段的
 *                   实例还没喂过，`age_ms` 会是"从开机到现在"。
 */
void fw_app_heartbeat(const struct app_ctx *ctx, uint32_t *seq, uint32_t *age_ms);

/** 这个实例是不是卡住了（心跳超过 `stale_ms` 没动）。
 *
 * @param ctx      目标实例；NULL 或不在 RUNNING → 返回 0。
 * @param stale_ms 判定阈值；0 → 返回 0（"0 毫秒没动"没有意义）。
 * @return 1 卡住；0 没卡住（或无从判断）。
 *         线程与中断：管理器任务里调；ISR 禁止。
 *         注意事项：判定为 1 **不代表框架能停它**——纯死循环不检查取消位，
 *                   取消请求对它无效。这个返回值是给管理器/产品仓库做决定用的。
 */
int fw_app_is_stalled(const struct app_ctx *ctx, uint32_t stale_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_APP_H */
