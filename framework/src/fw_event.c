/* framework/src/fw_event.c —— 事件（步骤 7.1）。
 *
 * 这是全框架唯一一条**反向**通道：平台在中断里 post，App 在任务里 wait。
 *
 * 无锁的依据是**单生产者**：每个 topic 只有一个上下文往里写（ISR 或某个任务，
 * 二选一）。消费者可以随便几个（tail 用 CAS-free 的自增，但为了不引入 ABA，
 * 这里也按"一个消费者"实现并在头文件里写清楚）。
 *
 * 为什么要拷贝：ISR 里填的那块缓冲，App 读之前可能已经被下一次中断覆盖。
 * 这种错误不会崩、不会报错，只是偶尔显示一帧旧数据——静默错误比崩溃贵。
 */
#include <string.h>

#include "framework/src/fw_internal.h"

struct fw_evt_slot {
    uint32_t len;
    unsigned char buf[FW_EVENT_MAX_BYTES];
};

struct fw_evt_topic {
    uint32_t head;    /* 下一个要写的序号（生产者独占） */
    uint32_t tail;    /* 下一个要读的序号（消费者独占） */
    uint32_t dropped; /* 因为满而丢弃的个数（诊断用） */
    size_t payload;   /* 声明时定下的负载大小；0 = 未声明 */
    struct fw_evt_slot slots[FW_EVENT_DEPTH];
};

/* 主题表。用途：每个 topic 的负载大小与环形队列（D28：一个 topic 一个生产者上下文）。
 * 谁写：`fw_event_declare()` 写 `payload`；生产者写 `head`、消费者写 `tail`，且各自
 *       只写自己的工作指针；`fw_event_forget()` 复位全部字段。
 * 并发约定：跨任务/ISR 的字段一律 `CM_ATOMIC_*`（写 RELEASE、读 ACQUIRE）；
 *           声明只在 App 任务里做，ISR 只读已声明的 `payload`。 */
static struct fw_evt_topic s_topics[FW_EVENT_TOPICS];

static struct fw_evt_topic *fw_topic_find(evt_topic_t topic)
{
    if (topic == 0u || topic > FW_EVENT_TOPICS) {
        return NULL;
    }
    struct fw_evt_topic *t = &s_topics[topic - 1u];

    return (t->payload == 0u) ? NULL : t;
}

cm_err_t fw_event_declare(evt_topic_t topic, size_t payload_bytes)
{
    struct fw_evt_topic *t;

    if (topic == 0u || topic > FW_EVENT_TOPICS) {
        return CM_ERR_INVALID;
    }
    if (payload_bytes == 0u || payload_bytes > FW_EVENT_MAX_BYTES) {
        return CM_ERR_INVALID;
    }
    t = &s_topics[topic - 1u];
    if (t->payload != 0u) {
        return CM_ERR_BUSY;
    }
    t->head = 0u;
    t->tail = 0u;
    t->dropped = 0u;
    CM_ATOMIC_STORE(&t->payload, payload_bytes, CM_MEM_RELEASE);
    return CM_OK;
}

/* 生产者侧：写槽位，然后**最后**发布 head。
 * 消费者先读 head（acquire），看到的槽位一定是写完的。 */
static cm_err_t fw_event_put(evt_topic_t topic, const void *data, size_t len)
{
    struct fw_evt_topic *t = fw_topic_find(topic);
    uint32_t h;
    uint32_t tl;
    struct fw_evt_slot *slot;

    if (t == NULL) {
        return CM_ERR_NOT_SUPPORTED;
    }
    if (data == NULL || len != CM_ATOMIC_LOAD(&t->payload, CM_MEM_ACQUIRE)) {
        return CM_ERR_INVALID;
    }
    h = CM_ATOMIC_LOAD(&t->head, CM_MEM_RELAXED);
    tl = CM_ATOMIC_LOAD(&t->tail, CM_MEM_ACQUIRE);
    if ((uint32_t)(h - tl) >= FW_EVENT_DEPTH) {
        (void)CM_ATOMIC_ADD_FETCH(&t->dropped, 1u, CM_MEM_RELAXED);
        return CM_ERR_BUSY;
    }
    slot = &t->slots[h % FW_EVENT_DEPTH];
    memcpy(slot->buf, data, len);
    CM_ATOMIC_STORE(&slot->len, (uint32_t)len, CM_MEM_RELAXED);
    CM_ATOMIC_STORE(&t->head, h + 1u, CM_MEM_RELEASE);
    return CM_OK;
}

cm_err_t fw_event_post(evt_topic_t topic, const void *data, size_t len)
{
    return fw_event_put(topic, data, len);
}

cm_err_t fw_event_post_isr(evt_topic_t topic, const void *data)
{
    struct fw_evt_topic *t = fw_topic_find(topic);
    size_t len;

    if (t == NULL) {
        return CM_ERR_NOT_SUPPORTED;
    }
    if (data == NULL) {
        return CM_ERR_INVALID;
    }
    /* 长度由声明决定：中断里不校验，也不返回长度参数。 */
    len = CM_ATOMIC_LOAD(&t->payload, CM_MEM_ACQUIRE);
    return fw_event_put(topic, data, len);
}

/* 消费者侧：读槽位，然后**最后**发布 tail。 */
static cm_err_t fw_event_get(evt_topic_t topic, void *buf, size_t cap, size_t *out_len)
{
    struct fw_evt_topic *t = fw_topic_find(topic);
    uint32_t tl;
    uint32_t h;
    uint32_t pending;
    size_t payload;
    struct fw_evt_slot *slot;

    if (t == NULL) {
        return CM_ERR_NOT_SUPPORTED;
    }
    payload = CM_ATOMIC_LOAD(&t->payload, CM_MEM_ACQUIRE);
    if (buf == NULL || cap < payload) {
        return CM_ERR_INVALID;
    }
    tl = CM_ATOMIC_LOAD(&t->tail, CM_MEM_RELAXED);
    h = CM_ATOMIC_LOAD(&t->head, CM_MEM_ACQUIRE);
    /* `head - tail` 是这个环的**唯一不变量**：0 = 空，1..DEPTH-1 = 有货。
     *
     * 两个生产者同时写同一个 topic（D28 禁止的事）时，`head` 可能被后写的
     * 生产者用旧值覆盖回去（A 读到 h=10 挂起 → B 推到 13 → A 存回 11）。
     * 这时 `head - tail` 会翻成一个巨大值。若这里只判 `tl == h`，队列看起来
     * "永远有货"，消费者会无限读下去——**测试挂死而不是变红**，这是最坏的失败形态。
     * 所以越界一律当成不变量被破坏，明确返回错误。 */
    pending = (uint32_t)(h - tl);
    if (pending == 0u) {
        return CM_ERR_TIMEOUT; /* "现在没有"，不是错误 */
    }
    if (pending > FW_EVENT_DEPTH) {
        return CM_ERR_STATE; /* 生产者违反了单写者前提：head 被回退过 */
    }
    /* `pending == FW_EVENT_DEPTH` 是**满**，不是坏——照常取。 */
    slot = &t->slots[tl % FW_EVENT_DEPTH];
    memcpy(buf, slot->buf, payload);
    if (out_len != NULL) {
        *out_len = payload;
    }
    CM_ATOMIC_STORE(&t->tail, tl + 1u, CM_MEM_RELEASE);
    return CM_OK;
}

cm_err_t fw_event_take(evt_topic_t topic, void *buf, size_t cap_bytes, size_t *out_len)
{
    return fw_event_get(topic, buf, cap_bytes, out_len);
}

cm_err_t fw_event_wait(evt_topic_t topic, void *buf, size_t cap_bytes, size_t *out_len,
                       uint32_t timeout_ms)
{
    fw_app_ctx_t *c = fw_current_ctx();
    uint32_t start;
    cm_err_t rc;

    if (fw_topic_find(topic) == NULL) {
        return CM_ERR_NOT_SUPPORTED; /* 先判主题：没这个能力比"没在 App 里"更具体 */
    }
    if (c == NULL || fw_ops() == NULL) {
        return CM_ERR_STATE; /* 没有取消位就没有"等"的资格 */
    }
    start = fw_ops()->clock_ms();
    for (;;) {
        rc = fw_event_get(topic, buf, cap_bytes, out_len);
        if (rc != CM_ERR_TIMEOUT) {
            return rc;
        }
        if ((uint32_t)(fw_ops()->clock_ms() - start) >= timeout_ms) {
            return CM_ERR_TIMEOUT;
        }
        /* 只在"还没到点"时睡一片；fw_delay_block 内部会查取消位，
         * 命中就 fw_run_exit() 不返回——所以这里不需要自己查。 */
        fw_delay_block(1u); /* 内部会喂心跳并查取消位 */
    }
}

void fw_event_forget(void)
{
    /* 全部走原子写：生产者可能还在提交（TSan 抓过一次：
     * `fw_event_forget` 的普通写撞上 `fw_event_put` 的原子写）。`fw_reset()` 的
     * 调用约定是"先停生产者再清表"，但检查器不该依赖调用方守规矩。 */
    for (uint32_t i = 0; i < FW_EVENT_TOPICS; i++) {
        CM_ATOMIC_STORE(&s_topics[i].head, 0u, CM_MEM_RELEASE);
        CM_ATOMIC_STORE(&s_topics[i].tail, 0u, CM_MEM_RELEASE);
        CM_ATOMIC_STORE(&s_topics[i].dropped, 0u, CM_MEM_RELEASE);
        CM_ATOMIC_STORE(&s_topics[i].payload, (size_t)0, CM_MEM_RELEASE);
    }
}
