/* api/af_api/evt.h —— 事件（App 可见的契约，D44）。
 *
 * 一个 topic 一个生产者上下文（D28）——这条前提是**契约的一部分**，不是实现细节：
 * 两个生产者往同一个 topic 里写，丢帧数量取决于时序，测试只能测到"这一轮没踩中"。
 *
 * 依赖约束同 `res.h`：只能 include `common/`。
 */
#ifndef ESP_AI_SKILL_AF_API_EVT_H
#define ESP_AI_SKILL_AF_API_EVT_H

#include "common/include/cm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 事件（步骤 7.1）──────────────────────────────────────────────
 *
 * 事件是**反向**的那条通道：平台在中断里报告"按键按下了""数据来了"，
 * 框架负责把它交给正在等的 App。§0.2 里 `platform → framework` 的唯一例外
 * 就是这两个 post 函数。
 *
 * 三条硬规矩（都不动，改了要重开版本）：
 *   1. **提交即拷贝**（D27，与 5.2 提交型接口同一条裁决）：`fw_event_post()`
 *      返回后调用方可以立刻重用缓冲区——否则中断服务程序里填的那块内存
 *      会在 App 读之前被下一次中断覆盖，而这种错误是**静默**的。
 *   2. **ISR 版无锁、无分配**（D28）：`fw_event_post_isr()` 只碰预分配的静态槽位，
 *      不做动态分配、不取互斥量、不写日志。它能做的工作量是常数。
 *   3. **一个 topic 只允许一个生产者上下文**：要么只在 ISR 里 post，要么只在一个
 *      任务里 post，不能两边都往同一个 topic 写（环形队列靠"单生产者"这个前提
 *      才能无锁；两个生产者请开两个 topic）。
 */

/** 事件主题号。0 永远无效（"没有主题"），从 1 开始。 */
typedef uint16_t evt_topic_t;

/** 一个主题里能排几个未读事件。
 *  满了之后 post 返回 `CM_ERR_BUSY` 并丢弃**新**事件——丢旧的会让正在处理的
 *  App 看到"历史被改写"，比丢新的更难查。 */
#define FW_EVENT_DEPTH 4u

/** 单个事件负载的上限。主题声明时给定自己的大小，不得超过它。 */
#define FW_EVENT_MAX_BYTES 32u

/** 同时能声明的主题数。 */
#define FW_EVENT_TOPICS 16u

/** 声明一个主题及其负载大小。**必须在该主题第一次 post 之前**完成。
 *
 * @param topic        主题号；0 → `CM_ERR_INVALID`。
 * @param payload_bytes 负载字节数；0 或 > `FW_EVENT_MAX_BYTES` → `CM_ERR_INVALID`。
 * @return `CM_OK`；`CM_ERR_INVALID` 参数越界；`CM_ERR_BUSY` 该主题已声明过；
 *         `CM_ERR_NOMEM` 主题表满了。
 *         线程与中断：**只能在 boot 阶段、任何一个任务开始 post 之前调**（通常是
 *         系统 App 的 `init()`）；ISR 禁止。
 *         注意事项：重复声明**不会**改大小，直接 BUSY——改大小意味着已经排队的
 *         事件全部作废，那应该是一个显式的 flush，不是偷偷换长度。
 */
cm_err_t fw_event_declare(evt_topic_t topic, size_t payload_bytes);

/** 投递一个事件（任务上下文）。
 *
 * @param topic 已声明的主题。
 * @param data  负载起点；`len` 必须等于声明的大小。
 * @param len   负载字节数。
 * @return `CM_OK`；`CM_ERR_NOT_SUPPORTED` 主题没声明；`CM_ERR_INVALID` `data` 为空
 *         或 `len` 与声明不符；`CM_ERR_BUSY` 队列满（事件被丢弃）。
 *         线程与中断：任务里调；**ISR 里必须用 `fw_event_post_isr()`**（这个版本
 *         会做长度校验与错误码，代价比 ISR 版大）。
 *         注意事项：提交即拷贝（D27），返回后 `data` 可立即重用。
 */
cm_err_t fw_event_post(evt_topic_t topic, const void *data, size_t len);

/** 在中断里投递一个事件。
 *
 * @param topic 已声明的主题。
 * @param data  负载起点；长度**由主题声明决定**，本函数不接收长度参数——
 *              中断里没有"顺便校验一下"的余量。
 * @return `CM_OK`；`CM_ERR_NOT_SUPPORTED` 主题没声明（中断早于声明就会这样，
 *         属于正常情况，不算错误）；`CM_ERR_BUSY` 队列满（丢弃）。
 *         线程与中断：**可以从 ISR 调用**；无锁、无分配、不记日志，工作量常数。
 *         注意事项：单生产者（见本节开头第 3 条）；返回 BUSY 时调用方通常什么
 *         都不该做——在中断里重试只会把中断拉长。
 */
cm_err_t fw_event_post_isr(evt_topic_t topic, const void *data);

/** 取一个事件，取不到就等。
 *
 * @param topic    主题。
 * @param buf      输出缓冲；`cap_bytes` 小于声明大小时返回 `CM_ERR_INVALID`。
 * @param cap_bytes 缓冲容量。
 * @param out_len  可为 NULL；实际写出的字节数。
 * @param timeout_ms 最多等多久；0 = 只试一次（等价 `fw_event_take`）。
 * @return `CM_OK`；`CM_ERR_NOT_SUPPORTED` 主题没声明；`CM_ERR_INVALID` 缓冲太小；
 *         `CM_ERR_TIMEOUT` 等到超时也没有事件；`CM_ERR_CANCELED` 等待期间 App 被停止。
 *         线程与中断：**只能在 App 任务里调**（需要取消位与切片等待）；ISR 禁止。
 *         注意事项：**这是伪阻塞点**——等待期间每隔 ≤20ms 检查一次取消位，
 *         命中后会走退出路径且**不返回**。所以 `fw_event_wait()` 之后的代码
 *         不一定会执行（与 `fw_delay_block()` 同一条规矩）。
 */
cm_err_t fw_event_wait(evt_topic_t topic, void *buf, size_t cap_bytes, size_t *out_len,
                       uint32_t timeout_ms);

/** 取一个事件，取不到立刻返回（不等待）。
 *
 * @param topic     主题。
 * @param buf       输出缓冲。
 * @param cap_bytes 缓冲容量。
 * @param out_len   可为 NULL；实际写出的字节数。
 * @return `CM_OK`；`CM_ERR_NOT_SUPPORTED` 主题没声明；`CM_ERR_INVALID` 缓冲太小；
 *         `CM_ERR_TIMEOUT` **队列里现在没有事件**（不是错误，是"还没有"）。
 *         线程与中断：任意任务可调；ISR 里也能调（无锁），但从 ISR 取事件
 *         通常说明方向反了——先想清楚为什么不是"投递 → 任务处理"。
 */
cm_err_t fw_event_take(evt_topic_t topic, void *buf, size_t cap_bytes, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_EVT_H */
