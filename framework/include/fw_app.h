/* framework/include/fw_app.h —— App 描述体与框架→App 的使用面。
 *
 * 这个头是"框架给 App 的词汇表"：App 长什么样（`app_desc_t`）、怎么被登记
 * （`fw_manifest_t`）、能调哪些东西（`fw_delay_block` / `fw_alloc` / `fw_free` /
 * `fw_log` / `fw_logp`）。
 *
 * 它与 `api/af_api/app.h` 的分工（2026-11-02 定，见 实施步骤.md §7 D11）：
 *   - **本头是唯一出处**：回调签名、`app_desc_t`、`fw_manifest_t`、使用面函数都在这里；
 *   - `api/af_api/app.h` 只做两件事：include 本头 + 定义 `struct app_ctx` 的可见成员。
 *   两边重复定义是"同一事实两个家"，早晚漂移；所以宁可让 App 的头多 include 一行。
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

/** App 上下文。**这里只有前向声明**——完整定义在 `api/af_api/app.h`，
 *  因为"App 能看见哪些槽位"是 App 侧的决定，不是框架侧的决定。 */
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

/* ── 使用面（App 能调的全部框架函数，v1 就这五个）───────────────── */

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
