/* api/af_api/svc.h —— 服务：一个 App 把自己的能力挂到名字上，别人按名字找它（D44）。
 *
 * 为什么 App 需要这一步：框架与 App 之间不许直接链接符号（两个 App 也不能互相
 * 链接）。所以"谁提供显示、谁提供存储"不能靠 `extern`，只能靠**注册表 + id**。
 * 三个码各自回答一个不同的问题，别混：
 *   - `CM_ERR_NOT_SUPPORTED`：这个 id 从没被注册过 → 这块产品没有这个能力；
 *   - `CM_ERR_UNAVAILABLE`  ：注册过，但 owner 已经退了/崩了 → 等它重启，可以重试；
 *   - `CM_OK`               ：owner 正在 RUNNING，现在能用。
 *
 * 提交型接口（`fw_svc_submit`）的规矩：
 *   **提交即拷贝**（D24）——`fw_svc_submit()` 返回后，调用方手上的缓冲区立刻可以重用。
 *   同 key 再提交 = 覆盖旧帧，旧帧的完成回调会收到 `superseded = 1`（"你这帧没被消费"）。
 *
 * 依赖约束同 `res.h`：只能 include `common/`。
 */
#ifndef ESP_AI_SKILL_AF_API_SVC_H
#define ESP_AI_SKILL_AF_API_SVC_H

#include "common/include/cm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 服务 id。**0 永远无效**（`FW_SVC_NONE`），合法范围 1..FW_SVC_MAX。 */
typedef uint8_t fw_svc_id_t;

/** 注册表容量。全框架静态表，线性查找——规模在几十个，不值得上哈希。 */
#define FW_SVC_MAX 64u

/** 无效服务 id。用它注册或查询一律 `CM_ERR_INVALID`。 */
#define FW_SVC_NONE ((fw_svc_id_t)0u)

/** 提交帧的完成回调。
 *
 * @param key        提交时用的 key，用于把回调对回那一帧
 * @param superseded 0 = 被服务取走了；1 = 还没被取就被同 key 的新帧顶掉了
 * @param user       提交时透传的上下文
 * @return 无。
 *         线程与中断：在**提交者自己**的上下文里回调（取走时是服务任务，覆盖时是提交者任务）；
 *                     ISR 里不会回调。
 *         注意事项：不要在回调里再做长时间阻塞，也不要再提交同一个 key（会覆盖自己）。
 */
typedef void (*fw_submit_done_fn)(uint32_t key, int superseded, void *user);

/** 把当前 App 注册成服务 `id` 的提供者。
 *
 * @param id 服务号；0 与 >FW_SVC_MAX 都非法（0 是 `FW_SVC_NONE`）。
 * @return CM_OK 成功；CM_ERR_INVALID id 为 0 或越界；
 *         CM_ERR_STATE 不在 App 任务里 / 同一 owner 重复注册；
 *         CM_ERR_BUSY  该 id 已被别的、还活着的 owner 占着。
 * 线程与中断：只能在 App 任务里调用（要"当前 App"是谁）；中断里禁止。
 * 注意事项：注册与退出是配对的——`fw_run_exit()` 第 ③ 步会把 owner 清空但**保留表项**，
 *           于是后续查询得到 `CM_ERR_UNAVAILABLE` 而不是 `NOT_SUPPORTED`（D23）。
 */
cm_err_t fw_svc_register(fw_svc_id_t id);

/** 注销当前 App 对 `id` 的注册（主动交还，通常在 `exit()` 里调）。
 *
 * @param id 服务号；必须是自己已经注册且仍然持有的那一个。
 * @return CM_OK 成功；CM_ERR_INVALID id 非法；CM_ERR_STATE 不在 App 任务里；
 *         CM_ERR_NOT_SUPPORTED 没有这个表项；CM_ERR_BUSY 表项属于别人。
 *         线程与中断：只能在 App 任务里调用；中断里禁止。
 *         注意事项：不注销也可以——退出时会自动清 owner。主动注销的用处是"我已经确定不再提供服务，
 *           别等我了"，此时表项被整体删除，查询回到 `CM_ERR_NOT_SUPPORTED`。
 */
cm_err_t fw_svc_unregister(fw_svc_id_t id);

/** 问一个服务现在能不能用（调用方在真正调用前查这一步）。
 *
 * @param id 服务号。
 * @return 见文件头三个码的说明。
 *         线程与中断：任意任务可调；中断里允许（不取锁、不分配）。
 * 注意事项：查完到真正调用之间 owner 仍可能退出——这是**快照**不是租约。
 */
cm_err_t fw_svc_ready(fw_svc_id_t id);

/** 提交一帧给服务 `svc`（提交型接口，立即返回）。
 *
 * @param svc   目标服务 id
 * @param key   同一服务内唯一；重复 key 表示"新的覆盖旧的"
 * @param data  数据指针，`len` 字节，**由框架拷贝**（返回后即可重用）
 * @param len   字节数，1..FW_SUBMIT_MAX_BYTES
 * @param done  完成回调，可为 NULL
 * @param user  回调上下文，可为 NULL
 * @return CM_OK；CM_ERR_INVALID 参数非法或超长；CM_ERR_NOT_SUPPORTED 服务没注册；
 *         CM_ERR_UNAVAILABLE owner 不可用；CM_ERR_BUSY 提交表满（**等会儿再来**，不是内存不足）。
 * 线程与中断：任意任务可调，内部只用 try-lock——**不会阻塞**；中断里禁止（会拷贝与分配）。
 * 注意事项：缓冲区归框架；不要在这之后再改 `data`（改了也影响不到框架那份，D24）。
 */
cm_err_t fw_svc_submit(fw_svc_id_t svc, uint32_t key, const void *data, size_t len,
                       fw_submit_done_fn done, void *user);

/** 服务侧取走一帧（提交型接口的另一半）。
 *
 * @param svc     本服务 id
 * @param out_key 输出：这一帧的 key，可为 NULL
 * @param buf     输出缓冲；`cap` 不够时**帧不动**，`*out_len` 返回所需长度，调用方可扩容重试
 * @param cap     缓冲容量
 * @param out_len 输出：实际（或所需）长度，可为 NULL
 * @return CM_OK；CM_ERR_NOT_SUPPORTED 队列空（没有待处理的帧，不是错误）；
 *         CM_ERR_INVALID 参数非法。
 *         线程与中断：服务任务里调用；中断里禁止。
 *         注意事项：取走即释放框架那份拷贝（D24），所以之后再读 `buf` 拿到的是你自己的数据。
 */
cm_err_t fw_svc_submit_take(fw_svc_id_t svc, uint32_t *out_key, void *buf, size_t cap,
                            size_t *out_len);

/** 某个服务当前待处理的帧数（诊断用）。
 *
 * @param svc 服务号。
 * @return 该服务名下还有几帧没被取走；服务不存在时返回 0。
 *         线程与中断：任意任务可调；中断里允许（只读一个计数）。
 *         注意事项：这是快照，只适合诊断与测试——**别拿它做同步**
 *                   （数完可能就被别的任务取走了）。
 */
uint32_t fw_svc_submit_count(fw_svc_id_t svc);

/* 单帧上限。超过 `fw_svc_submit()` 直接 `CM_ERR_INVALID`，不截断——截断是静默错误。 */
#define FW_SUBMIT_MAX_BYTES 4096u

#ifdef __cplusplus
}
#endif
#endif /* ESP_AI_SKILL_AF_API_SVC_H */
