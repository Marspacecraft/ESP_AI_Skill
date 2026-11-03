/* framework/src/fw_internal.h —— 框架内部接口（**不是 App 的词汇表**）。
 *
 * 为什么放在 `src/` 而不是 `include/`：`framework/include/` 是 tag 1/2 的
 * link 面（见 policy.txt），那里出现 `pf_ops_t` 就等于把平台表塞进 App 的
 * 编译单元，`fw_app.h` 那条"只准 include common/"的硬约束当场作废。
 * 所以凡是签名里带 `pf_ops_t` / `pf_manifest` / 注册表的东西，一律住这里。
 *
 * 谁能 include 它：框架自己的 .c 源文件，以及 tests/framework/ 下的测试（测试要
 * 直接驱动真框架，见 1.4 的"诚实边界"）。App、平台、api 都不该看见它。
 *
 * 线程与中断：本文件里的函数全部只能在框架初始化阶段（单线程、无并发）调用；
 * 没有任何一个是可重入或 ISR 安全的。
 */
#ifndef ESP_AI_SKILL_FW_INTERNAL_H
#define ESP_AI_SKILL_FW_INTERNAL_H

#include "common/include/cm_types.h"
#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 平台表 ────────────────────────────────────────────────────── */

/** 校验一张平台表是否可被框架接受。
 *
 * @param ops 平台表；不得为 NULL。
 * @return `CM_OK` 可用；`CM_ERR_INVALID` 表不完整（九项有 NULL）或 caps 保留位
 *         非 0；`CM_ERR_VERSION` 版本不匹配。失败时*不*保留任何状态。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：会**调用** `ops->caps()`，因此调用前该表必须已经填好。
 */
cm_err_t fw_ops_check(const pf_ops_t *ops);

/** 绑定平台表。整机只能绑一次，且必须早于 `fw_init()`。
 *
 * @param ops 平台表；生命周期必须覆盖整个运行期（框架只存指针，不拷贝）。
 * @return `CM_OK`；`CM_ERR_STATE` 已绑过或已 `fw_init()`；其余同 `fw_ops_check()`。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：绑定失败不会留下半绑定状态。
 */
cm_err_t fw_platform_bind(const pf_ops_t *ops);

/** 取已绑定的平台表。未绑定时返回 NULL。
 *  线程与中断：初始化阶段与运行期均可读；返回的指针只读。 */
const pf_ops_t *fw_ops(void);

/* ── 注册表 ────────────────────────────────────────────────────── */

/** 合并 N 份清单进注册表并校验。
 *
 * @param manifests 清单数组；为 NULL 或 `count==0` 返回 `CM_ERR_INVALID`。
 * @param count     清单份数。
 * @return `CM_OK` 注册完成；`CM_ERR_STATE` 未绑平台表或已初始化过；
 *         `CM_ERR_INVALID` 任一条目不合法（见下）或**所有清单合计为 0 个 App**。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：失败时注册表回到空（不会留下半份清单）。单条目合法条件：
 *                   id ∈ [2,254]、`name`/`init`/`exit`/`task` 非 NULL、
 *                   `flags` 只有 `APP_FLAG_SYSTEM`、`stack_bytes >= APP_STACK_MIN`、
 *                   **id 不得重复**。空清单（`count==0` 的那一份）合法，跳过。
 */
/** 启动序列的可测部分：bind → init → boot（步骤 4.1）。
 *
 * @param ops 平台表，不得为 NULL；为 NULL 时直接 `CM_ERR_INVALID`（不尝试用空表启动）。
 * @param man App 清单，不得为 NULL；`count == 0` 由 `fw_init()` 判 `CM_ERR_INVALID`。
 * @return bind/init/boot 三段的第一个失败码。
 *         线程与中断：只在启动阶段（`app_main()` 里）调用一次；ISR 禁止。
 *         注意事项：**全进程只该调一次**——第二次会因为"已绑表/已 boot"返回
 *                   `CM_ERR_STATE`。它的存在只为让测试能验启动顺序；
 *                   生产路径是 `app_main()` 一个人走完。
 */
cm_err_t fw_startup(const pf_ops_t *ops, const fw_manifest_t *man);

cm_err_t fw_init(const fw_manifest_t *manifests, uint32_t count);

/** 已注册的 App 个数。未初始化时返回 0。
 *  线程与中断：初始化后可读；ISR 里读到的是稳定的常量。 */
uint32_t fw_app_count(void);

/** 按注册顺序取第 index 个描述体；越界返回 NULL。
 *  线程与中断：初始化后可读；返回的指针指向静态常量，不要改。 */
const app_desc_t *fw_app_at(uint32_t index);

/** 清空注册表与平台绑定，回到"未初始化"。**只给测试用**——真机上没有第二次启动。
 *  线程与中断：无任务运行时才可调。 */
void fw_reset(void);

/* ── id 发号器 ─────────────────────────────────────────────────── */

/** 领一个**从未用过**的 id（D18：重启不复用旧 id）。
 *
 * @param out 输出参数；不得为 NULL。
 * @return `CM_OK` 已写入 `[2,254]` 内最小的空闲 id 并标记为已用；
 *         `CM_ERR_NOMEM` 号码发完了（253 个全用光）。失败时 `*out` 不变。
 *         线程与中断：仅管理器/测试在"要新建实例"时调用；ISR 禁止。
 *         注意事项：**没有对应的释放函数**——id 只增不减。复用旧 id 会让
 *                   拿着它的人静默操作到新实例上，代价远大于号码耗尽。
 */
cm_err_t fw_alloc_id(uint32_t *out);

/* ── 启动 ──────────────────────────────────────────────────────── */

/** 拉起所有 `APP_FLAG_SYSTEM` 的 App（阶段 3 只做这一半）。
 *
 * @return `CM_OK` 该起的都起了；`CM_ERR_STATE` 未 `fw_init()` 或已经 boot 过；
 *         平台拒绝建任务时返回它的错误码，已起的保持运行（不做回滚——把已经
 *         跑起来的任务杀掉比留着更难解释）。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：业务 App 由管理器用 `fw_app_ctx_new_with_id()` +
 *                   `fw_app_start()` 自行拉起，`fw_boot()` 不管它们。
 */
cm_err_t fw_boot(void);

/** `fw_boot()` 拉起并仍在表里的实例个数。 */
uint32_t fw_live_count(void);

/** 第 index 个活实例的上下文；越界返回 NULL。 */
struct fw_app_ctx *fw_live_at(uint32_t index);

/** 把一个实例从活实例表里摘掉（**不释放它**，见 D19）。
 *
 * @param c `fw_live_at()` 拿到的指针；不在表里时无操作。
 *         线程与中断：管理器在自己任务里调；ISR 禁止。
 *         注意事项：先 `remove` 再 `fw_app_ctx_free()`——顺序反了就会在表里
 *                   留下一根悬垂指针，`fw_live_at()` 之后读到的是野内存。
 *                   **只能对已经进 STOPPED/FAILED 的实例调用**；还在跑就摘表，
 *                   等于把"谁负责停它"这件事弄丢了。
 */
void fw_live_remove(struct fw_app_ctx *c);
/* ── 服务注册表（步骤 5.1）──────────────────────────────────────
 *
 * 一个"服务"= 某个 App 对外提供的、别人可以按名字找过来的能力（存储、显示、
 * 网络……）。表里只有三件事：**谁提供**（owner）、**在不在**（registered）、
 * **能不能用**（owner 的状态）。
 *
 * 三条判定（`fw_svc_ready()` 的返回值，顺序就是优先级）：
 *   - 表里没有          → `CM_ERR_NOT_SUPPORTED`（这块板/这个产品没这个能力）
 *   - 有，但 owner 没在跑（或已死、表项还在）→ `CM_ERR_UNAVAILABLE`（**可以重试**）
 *   - 有且在跑          → `CM_OK`
 *
 * 为什么"没注册"与"注册了但崩了"必须分开：前者重试一万次也没用（该改的是
 * 产品配置），后者重试是对的（管理器可能正在重启它）。合成一个错误码，
 * 调用方就只有"放弃"一个选项。
 */

/** 服务 id。**0 永远无效**；具体编号是编译期契约，加服务 = 加编号（见 §7）。 */
typedef uint8_t fw_svc_id_t;

/** 服务表容量。64 个服务在 v1 已经远超需要；表是静态的，不占堆。 */
#define FW_SVC_MAX 64u

/** 注册一个服务。**只能由服务自己的 App 调**，且必须在自己还 RUNNING 时调。
 *
 * @param id    服务 id，`1..FW_SVC_MAX-1`。
 * @param owner 提供者上下文，不得为 NULL；通常是 `fw_current_ctx()`。
 * @return `CM_OK`；`CM_ERR_INVALID` 参数非法或 id 越界；
 *         `CM_ERR_BUSY` 已被**别的** owner 占着；
 *         `CM_ERR_STATE` 自己已经注册过（重复注册是 App 的 bug，不是幂等操作）。
 *         **表项还在但 owner 已经死了时，注册是"认领"**（返回 `CM_OK`）——那就是重试成功。
 *         线程与中断：App 任务里调；ISR 禁止。无阻塞、无伪阻塞语义
 *                     （**不会因为取消位而结束当前任务**）。
 *         注意事项：App 退出时**表项保留、owner 清空**（`fw_run_exit()` 的 ③ 步）——
 *                   保留是为了让调用方看到 UNAVAILABLE（该重试）而不是
 *                   NOT_SUPPORTED（该放弃）。所以 `exit()` 里**不需要**调
 *                   `fw_svc_unregister()`；想彻底摘掉用 `fw_svc_unregister()`。
 */
cm_err_t fw_svc_register(fw_svc_id_t id, struct fw_app_ctx *owner);

/** 摘掉一个服务。
 *
 * @param id    服务 id。
 * @param owner 必须与注册时一致，否则 `CM_ERR_STATE`（防止 A 把 B 的服务摘了）。
 * @return `CM_OK`；未注册 → `CM_ERR_NOT_SUPPORTED`；owner 不符 → `CM_ERR_STATE`。
 *         线程与中断：App 任务里调；ISR 禁止。非阻塞。
 */
cm_err_t fw_svc_unregister(fw_svc_id_t id, struct fw_app_ctx *owner);

/** 谁在提供这个服务。
 *
 * @param id 服务 id。
 * @return owner 指针；未注册返回 NULL。
 *         线程与中断：任意任务可调；ISR 禁止（表项可能正在被另一个任务改）。
 *         注意事项：返回的指针可能在下一次 `fw_run_exit()` 之后失效——
 *                   要用它就得先确认对方还在跑（`fw_app_state()`）。
 */
struct fw_app_ctx *fw_svc_owner(fw_svc_id_t id);

/** 这个服务现在能不能用。
 *
 * @param id 服务 id。
 * @return `CM_OK` / `CM_ERR_NOT_SUPPORTED` / `CM_ERR_UNAVAILABLE`（见上）。
 *         线程与中断：任意任务可调；ISR 禁止。**非阻塞、不查取消位**——
 *                    它只是查表，没有"等"，所以不适用伪阻塞那套。
 */
cm_err_t fw_svc_ready(fw_svc_id_t id);

/** 把某个 owner 注册的服务全部摘掉。**只给 `fw_run_exit()` 用**。 */
void fw_svc_drop_owner(struct fw_app_ctx *owner);

/** 清空服务表。**只给 `fw_reset()` 用**。 */
void fw_svc_forget(void);

/* ── 提交型接口（步骤 5.2）──────────────────────────────────────
 *
 * 形态：调用方把一帧数据交给服务，**立即返回**，不等它被消费。屏幕刷新、
 * 网络发送都是这个形状——调用方是 App 的任务，不能为了等外设而把 200ms
 * 的预算耗掉。
 *
 * **所有权（D24，本步骤唯一的裁决）：提交即拷贝。**
 * `fw_svc_submit()` 返回后，调用方手上的那块缓冲区就可以立刻重用——框架
 * 在提交时已经拷了一份自己的。代价是每帧一份拷贝 + 一份堆内存，换来的是
 * "调用方忘了"这类错误**不可能发生**：没有共享期，就没有共享期违约。
 *
 * 备选方案与被否的理由：
 *   - (b) 零拷贝 + "完成事件之前不许改"：省一次拷贝，但违约是"画面撕裂"
 *     这种**静默**错误，只有在真面板上才会显形，而且发生在别人的任务里。
 *   - (c) 双缓冲：调用方持有 N 块轮转。省拷贝，但把"什么时候能写第 k 块"
 *     这个状态机推给了每个 App——那正是本项目要消灭的东西。
 *   v1 没有屏幕、也没有任何提交型服务，先按 (a) 把语义钉死；(b) 记在
 *   §6.7，等真有吞吐压力时再谈。
 */
typedef void (*fw_submit_done_fn)(uint32_t key, int superseded, void *user);

/** 单帧上限。超过就是 `CM_ERR_INVALID`——**不许悄悄截断**。 */
#define FW_SUBMIT_MAX_BYTES 4096u

/** 提交表容量（全框架共用，不是每服务一份）。8 帧已经够 v1；表是静态的。 */
#define FW_SUBMIT_SLOTS 8u

/** 提交一帧给某个服务。
 *
 * @param svc  目标服务 id；必须已注册且 owner 正在跑。
 * @param key  帧的标识。**同一个 key 再提交，会顶掉还没被取走的那一帧**。
 * @param data 数据；不得为 NULL。
 * @param len  字节数；`1..FW_SUBMIT_MAX_BYTES`。
 * @param done 完成回调，可为 NULL。`superseded` 非 0 表示这一帧**没被消费**，
 *             是被同 key 的新帧顶掉或被 `fw_reset()` 丢掉的。
 * @param user 原样回传给 `done`。
 * @return `CM_OK`；`CM_ERR_INVALID` 参数非法；`CM_ERR_NOT_SUPPORTED` 服务没注册；
 *         `CM_ERR_UNAVAILABLE` 服务已死；`CM_ERR_NOMEM` 拷贝分配失败或没有空槽；
 *         `CM_ERR_BUSY` 提交表正被另一个任务使用（**不等待，试一次就走**）。
 *         线程与中断：App 任务里调；ISR 禁止。非阻塞（不睡、不查取消位）。
 *         注意事项：`data` 在返回后即可释放；`done` 在**取帧的任务**里同步调用，
 *                   不要在回调里再调 `fw_svc_submit_take()`（会拿到 BUSY）。
 */
cm_err_t fw_svc_submit(fw_svc_id_t svc, uint32_t key, const void *data, size_t len,
                       fw_submit_done_fn done, void *user);

/** 取一帧。由服务的 owner 任务调。
 *
 * @param svc     服务 id。
 * @param out_key 输出该帧的 key；可为 NULL。
 * @param buf     接收缓冲区。
 * @param cap     `buf` 的容量。**cap < 帧长时返回 `CM_ERR_INVALID` 且帧留在表里**，
 *                `out_len` 仍会被写成这一帧需要的字节数（方便调用方扩容重试）。
 * @param out_len 输出帧长；可为 NULL。
 * @return `CM_OK`；`CM_ERR_INVALID` 参数非法或容量不够；`CM_ERR_NOT_SUPPORTED`
 *         这个服务没有任何待取的帧（**空队列不是错误码 NOMEM**）。
 *         线程与中断：owner 任务里调；ISR 禁止。非阻塞。
 *         注意事项：取走后框架那份拷贝被释放，`done(key, 0, user)` 同步触发。
 */
cm_err_t fw_svc_submit_take(fw_svc_id_t svc, uint32_t *out_key,
                            void *buf, size_t cap, size_t *out_len);

/** 这个服务还有几帧没被取走。调试与测试用。 */
uint32_t fw_svc_submit_count(fw_svc_id_t svc);

/** 建提交表的锁。**只给 `fw_init()` 用**；返回非 `CM_OK` 表示平台建不出锁。 */
cm_err_t fw_submit_init(void);

/** 丢掉所有待取帧（各自触发 `done(key, 1, user)`）并清锁。**只给 `fw_reset()`。** */
void fw_submit_forget(void);


/** 清空活实例表。**只给 `fw_reset()` 用**。 */
void fw_boot_forget(void);

/* ── App 运行期上下文与伪阻塞 ──────────────────────────────────── */

/** 伪阻塞的单片长度上限（毫秒）。架构约束是"单段不可打断等待 < 200ms"，
 *  框架取 20ms——留 10 倍余量给平台调度抖动。 */
#define FW_DELAY_SLICE_MS 20u

/** App 的六个状态。`EMPTY` 只在上下文刚分配、任务还没起来时出现。 */
typedef enum fw_app_state_e {
    FW_APP_STATE_EMPTY = 0,
    FW_APP_STATE_INIT,
    FW_APP_STATE_RUNNING,
    FW_APP_STATE_STOPPING,
    FW_APP_STATE_STOPPED,
    FW_APP_STATE_FAILED
} fw_app_state_e;

/** 框架侧的 App 上下文。App 拿到的 `app_ctx_t*` 就是它的 `pub` 成员。
 *
 * `cancel` / `state` / `exit_calls` / `in_exit` 是**跨任务可见**的：任务自己写，
 * 别的任务（管理器、测试、退出路径）读。所以它们必须 volatile，且只能用
 * "写一个 32 位整数"的表达——没有锁，也不该有锁。
 */
struct fw_app_ctx {
    app_ctx_t pub;                  /* 必须是第一个成员：App 拿到的是 &pub */
    const app_desc_t *desc;
    uint32_t cancel;                /* 取消位：1 = 有停止请求 */
    uint32_t state;                 /* fw_app_state_e */
    uint32_t exit_calls;            /* exit() 被调用过几次（只该是 0 或 1） */
    uint32_t in_exit;               /* 1 = 正在跑 exit()：取消位失效 */
    res_kind_e slot_kind[APP_CTX_SLOTS]; /* 每个槽位的资源种类（槽位为空时无意义） */
    res_life_e slot_life[APP_CTX_SLOTS]; /* 生命周期：决定退出路径管不管它 */
    uint32_t slots_released;             /* 退出路径回收了几个槽位（测试用） */
    size_t used_bytes;                   /* 记账：本实例当前占用的堆字节数（原子） */
    size_t quota_bytes;                  /* 0 = 不限 */
    uint32_t denied_count;               /* 因配额被拒的次数（原子） */
};

/** 退出路径第 ③ 步：回收所有 `RES_LIFE_APP` 的槽位，恰好一次。
 *  只给 `fw_run_exit()` 用。`RES_LIFE_BOOT` 的槽位**不动**（不是我们的东西）。 */
void fw_res_release_all(struct fw_app_ctx *c);

/** 要错误码的分配：区分 `CM_ERR_QUOTA`（你超了）与 `CM_ERR_NOMEM`（整机没了）。
 *  只给框架内部（`res_acquire`）用，App 侧只有 `fw_alloc`。 */
void *fw_alloc_ex(struct fw_app_ctx *c, size_t bytes, cm_err_t *out);

/** 试着为 `bytes` 预留配额（原子地加，超额再退回）。
 *
 * @return 1 已预留；0 超配额（调用方该返回 `CM_ERR_QUOTA` / NULL）。
 *         线程与中断：App 任务里调；ISR 禁止。
 */
int fw_quota_reserve(struct fw_app_ctx *c, size_t bytes);

/** 归还 `bytes` 配额。**只给 `fw_free()` 与资源回收路径用**。 */
void fw_quota_release(struct fw_app_ctx *c, size_t bytes);

/** 这个实例在退出路径上回收了几个槽位。测试与诊断用。 */
uint32_t fw_app_slots_released(const struct fw_app_ctx *c);

/* 跨任务读写的四个字段**必须**经下面三个函数访问。
 *
 * 为什么不用 `volatile`：`volatile` 只挡编译器优化，不提供原子性，也不给
 * 线程模型任何"这件事发生在那件事之前"的承诺——C11 里它仍是数据竞争（UB）。
 * 在多核 ESP32 上，编译器把一次 `cancel=1` 重排到 `state=RUNNING` 之后是完全
 * 合法的，现场表现是"请求了停止但状态还在跑"。
 *
 * 为什么不用 `<stdatomic.h>`：与 `common/` 的选择一致——老工具链对 `_Atomic`
 * 支持不全，而 `__atomic_*` 是 GCC/Clang 共有的、32 位无需 libatomic 的路径。
 * 迁移到 `atomic_uint` 时不改语义，只改写法。
 *
 * 用哪一档序：
 *   - 取消位/状态用 ACQUIRE/RELEASE：写的一方"把话说出去"，读的一方必须看见
 *     写之前发生的事（否则读到 STOPPED 却读不到 exit 里写的日志/计数）。
 *   - 计数器用 RELAXED：只要求"整个数"，不要求同步别人。
 */
static inline uint32_t fw_atomic_load_u32(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void fw_atomic_store_u32(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/** 置 1 并返回**旧值**：`exit_calls` 的"恰好一次"靠它。 */
static inline uint32_t fw_atomic_exchange_u32(uint32_t *p, uint32_t v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL);
}

/** 分配一个上下文并做静态初始化，id 取 `desc->id`（首次启动用这个）。
 *
 * @param desc 注册表里那份描述体；不得为 NULL，`desc->id` 已在 `fw_init()` 校验过。
 * @return 上下文指针；平台表未绑或内存不足返回 NULL。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：用 `fw_ops()->mem_alloc` 分配，必须 `fw_app_ctx_free()` 释放；
 *                   失败时不留任何状态。
 */
struct fw_app_ctx *fw_app_ctx_new(const app_desc_t *desc);

/** 同 `fw_app_ctx_new()`，但**显式指定实例 id**（重启用这个，D18）。
 *
 * @param desc 描述体；不得为 NULL。
 * @param id   实例 id，必须 ∈ `[2,254]`，且必须是 `fw_alloc_id()` 发出来的。
 * @return 上下文指针；参数非法、平台表未绑或内存不足返回 NULL。
 *         线程与中断：仅管理器/测试侧；ISR 禁止。
 *         注意事项：**不会**检查 id 是否已被别人用掉——那是 `fw_alloc_id()`
 *                   的职责；这里只保证范围合法。新实例的 id 必须与旧实例不同。
 */
struct fw_app_ctx *fw_app_ctx_new_with_id(const app_desc_t *desc, uint32_t id);

/** 释放上下文。**只能在任务已经结束之后调**（任务还在跑就释放 = UAF）。 */
void fw_app_ctx_free(struct fw_app_ctx *c);

/** 为上下文创建任务，任务里依次跑 `init()` → `task()`。
 *
 * @param c `fw_app_ctx_new()` 的产物。
 * @return `CM_OK` 任务已创建；`CM_ERR_NOMEM` 平台拒绝建任务；`CM_ERR_STATE`
 *         `c` 为 NULL 或已经启动过。
 *         线程与中断：仅初始化阶段；ISR 禁止。
 *         注意事项：任务入口会把 `state` 推成 INIT→RUNNING；`init()` 失败或
 *                   `task()` 返回都会走退出路径（`fw_run_exit`），那条路径
 *                   **不返回**，所以这里看到的 `state` 之后还会变。
 */
cm_err_t fw_app_start(struct fw_app_ctx *c);

/** 当前上下文的状态（`fw_app_state_e`）。`c` 为 NULL 时返回 `FW_APP_STATE_EMPTY`。 */
fw_app_state_e fw_app_state(const struct fw_app_ctx *c);

/** `exit()` 至今被调用过几次。正常是 1（含启动失败）。 */
uint32_t fw_app_exit_calls(const struct fw_app_ctx *c);

/** 置取消位。可以在别的任务里调，也可以在测试里调。
 *  线程与中断：可以跨任务；ISR 里能调（只写一个整数），但不在 v1 承诺范围内。 */
void fw_app_request_cancel(struct fw_app_ctx *c);

/** 取**当前任务**的上下文；不在 App 任务里时返回 NULL。
 *  线程与中断：靠 C11 `_Thread_local` 实现，每个任务各一份；ISR 里返回 NULL。 */
struct fw_app_ctx *fw_current_ctx(void);

/** 走完退出路径并结束当前任务——**永不返回**。
 *
 * @param c         当前任务的上下文。
 * @param end_state 终态：`STOPPED`（被取消/被停止）或 `FAILED`（init 失败或
 *                  task 返回）。
 *         线程与中断：只能在 App 任务里调；ISR 禁止。
 *         注意事项：五步顺序是契约——①先清取消位（否则 exit() 里的阻塞会再
 *                   命中一次、永不结束）②`exit()` 恰好调用一次
 *                   ③（阶段 6 才有的资源回收）④置终态 ⑤结束任务。
 */
void fw_run_exit(struct fw_app_ctx *c, fw_app_state_e end_state)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noreturn))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_FW_INTERNAL_H */
