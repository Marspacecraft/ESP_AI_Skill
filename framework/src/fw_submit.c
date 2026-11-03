/* framework/src/fw_submit.c —— 提交型接口（步骤 5.2）。
 *
 * 一句话：**提交即拷贝**。调用方把数据交出去就能走，框架拿一份自己的。
 *
 * 为什么不是零拷贝：零拷贝要求"从提交到完成之前别改这块内存"，而违反它的
 * 后果是画面撕裂、数据错位这类**静默**错误——不崩、不报错，只是内容偶尔不对。
 * 这类错误恰好落在"AI 写代码"最不可靠的地方（跨任务的时序契约），所以宁可
 * 花一次 memcpy。
 *
 * 锁：整张表一把平台互斥锁。**只用 try-lock**（`mutex_take(m, 0)`），拿不到
 * 就返回 `CM_ERR_BUSY`。理由：这两个函数都能被 App 任务调用，一旦在这里
 * 等待，就得回答"等多久算违约、取消位要不要查"——那是伪阻塞那套纪律，而
 * 它属于真正的阻塞点（`fw_delay_block`），不该被一张 8 项的查询表拖进来。
 */
#include "framework/src/fw_internal.h"

struct submit_slot {
    fw_svc_id_t svc;
    uint32_t key;
    unsigned char *buf;   /* 框架自己的拷贝，取走/顶掉时释放 */
    size_t len;
    fw_submit_done_fn done;
    void *user;
    int used;
};

static struct submit_slot s_slot[FW_SUBMIT_SLOTS];
static pf_mutex_t *s_lock;

static cm_err_t lock_try(void)
{
    const pf_ops_t *ops = fw_ops();

    if (ops == NULL || s_lock == NULL) {
        return CM_ERR_STATE;      /* 没 init 就提交：调用方顺序错了 */
    }
    if (ops->mutex_take(s_lock, 0u) != 0) {
        return CM_ERR_BUSY;
    }
    return CM_OK;
}

static void unlock(void)
{
    const pf_ops_t *ops = fw_ops();

    if (ops != NULL) {
        ops->mutex_give(s_lock);
    }
}

/* 调用方必须已经持有锁。 */
static void slot_clear(struct submit_slot *slot, int superseded)
{
    const pf_ops_t *ops = fw_ops();

    if (slot->buf != NULL && ops != NULL) {
        ops->mem_free(slot->buf);
    }
    if (slot->done != NULL) {
        slot->done(slot->key, superseded, slot->user);
    }
    slot->svc = 0u;
    slot->key = 0u;
    slot->buf = NULL;
    slot->len = 0u;
    slot->done = NULL;
    slot->user = NULL;
    slot->used = 0;
}

cm_err_t fw_submit_init(void)
{
    const pf_ops_t *ops = fw_ops();

    if (ops == NULL) {
        return CM_ERR_STATE;
    }
    if (s_lock != NULL) {
        return CM_OK;             /* 已经建过：重复 init 会先被 fw_init 挡住 */
    }
    s_lock = ops->mutex_create();
    /* 建不出锁 = 平台违约（九项里 mutex_create 承诺只有资源耗尽才返回 NULL），
     * 但这里不能断言，只能让 init 失败：一个不能保证互斥的提交表比没有更糟。 */
    return (s_lock == NULL) ? CM_ERR_INVALID : CM_OK;
}

cm_err_t fw_svc_submit(fw_svc_id_t svc, uint32_t key, const void *data, size_t len,
                       fw_submit_done_fn done, void *user)
{
    const pf_ops_t *ops = fw_ops();
    struct submit_slot *slot = NULL;
    unsigned char *copy;
    cm_err_t rc;
    uint32_t i;

    if (svc == 0u || data == NULL || len == 0u || len > FW_SUBMIT_MAX_BYTES) {
        return CM_ERR_INVALID;
    }
    /* 提交型接口是"让出点"之一：调用方走完一次提交就算又前进了一步。 */
    fw_heartbeat_touch(fw_current_ctx());
    /* 服务判定沿用 5.1：没注册是 NOT_SUPPORTED，注册了但死了是 UNAVAILABLE。
     * 在锁外面查：`fw_svc_ready()` 自己不拿锁，也不会和提交表竞争。 */
    rc = fw_svc_ready(svc);
    if (rc != CM_OK) {
        return rc;
    }
    if (ops == NULL) {
        return CM_ERR_STATE;
    }

    /* 先拷后进锁：拷贝可能失败（没有内存），而"拿锁—分配—失败—放锁"这条路径
     * 会让一次纯粹的失败去挤掉别人一次成功。 */
    copy = (unsigned char *)ops->mem_alloc(len);
    if (copy == NULL) {
        return CM_ERR_NOMEM;
    }
    for (i = 0; i < (uint32_t)len; i++) {
        copy[i] = ((const unsigned char *)data)[i];
    }

    rc = lock_try();
    if (rc != CM_OK) {
        ops->mem_free(copy);      /* 进不去就不留半成品 */
        return rc;
    }

    /* 同 key 覆盖：先把旧帧摘掉（释放它的拷贝并通知它"没被消费"）。 */
    for (i = 0; i < FW_SUBMIT_SLOTS; i++) {
        if (s_slot[i].used && s_slot[i].svc == svc && s_slot[i].key == key) {
            slot_clear(&s_slot[i], 1);
            slot = &s_slot[i];
            break;
        }
    }
    if (slot == NULL) {
        for (i = 0; i < FW_SUBMIT_SLOTS; i++) {
            if (!s_slot[i].used) {
                slot = &s_slot[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        unlock();
        ops->mem_free(copy);
        /* **表满是 BUSY，不是 NOMEM**。八格全占着说明服务那边还没消费完，
         * 调用方该等一下再试；报 NOMEM 会让人去查一个并不存在的内存泄漏。
         * 这条是 9.4 的真机用例发现的：host 上没人把表填满过。 */
        return CM_ERR_BUSY;
    }

    slot->svc = svc;
    slot->key = key;
    slot->buf = copy;
    slot->len = len;
    slot->done = done;
    slot->user = user;
    slot->used = 1;

    unlock();
    return CM_OK;
}

cm_err_t fw_svc_submit_take(fw_svc_id_t svc, uint32_t *out_key,
                            void *buf, size_t cap, size_t *out_len)
{
    const pf_ops_t *ops = fw_ops();
    struct submit_slot *slot = NULL;
    cm_err_t rc;
    uint32_t i;

    if (svc == 0u || buf == NULL) {
        return CM_ERR_INVALID;
    }
    if (ops == NULL) {
        return CM_ERR_STATE;
    }

    rc = lock_try();
    if (rc != CM_OK) {
        return rc;
    }
    for (i = 0; i < FW_SUBMIT_SLOTS; i++) {
        if (s_slot[i].used && s_slot[i].svc == svc) {
            slot = &s_slot[i];
            break;
        }
    }
    if (slot == NULL) {
        unlock();
        return CM_ERR_NOT_SUPPORTED;   /* 没有待取的帧 */
    }
    if (cap < slot->len) {
        /* 容量不够：**帧不动**，把需要的长度告诉调用方，让它扩容再来。 */
        if (out_len != NULL) {
            *out_len = slot->len;
        }
        unlock();
        return CM_ERR_INVALID;
    }

    if (out_key != NULL) {
        *out_key = slot->key;
    }
    if (out_len != NULL) {
        *out_len = slot->len;
    }
    for (i = 0; i < (uint32_t)slot->len; i++) {
        ((unsigned char *)buf)[i] = slot->buf[i];
    }
    /* 先拷出去再清：`slot_clear()` 会释放 `slot->buf` 并回调，回调里读到的是
     * 调用方的缓冲区，不是框架的。 */
    slot_clear(slot, 0);
    unlock();
    return CM_OK;
}

uint32_t fw_svc_submit_count(fw_svc_id_t svc)
{
    uint32_t i;
    uint32_t n = 0u;

    if (svc == 0u || lock_try() != CM_OK) {
        return 0u;
    }
    for (i = 0; i < FW_SUBMIT_SLOTS; i++) {
        if (s_slot[i].used && s_slot[i].svc == svc) {
            n++;
        }
    }
    unlock();
    return n;
}

void fw_submit_forget(void)
{
    const pf_ops_t *ops = fw_ops();
    uint32_t i;

    /* 收尾路径可能发生在锁已经不可用的世界里（reset 之前平台表被清过），
     * 所以这里不拿锁：整机马上要回到空表，没有别的任务还该在提交。 */
    for (i = 0; i < FW_SUBMIT_SLOTS; i++) {
        if (s_slot[i].used) {
            slot_clear(&s_slot[i], 1);
        }
    }
    (void)ops;
    s_lock = NULL;   /* 平台没有 destroy，锁对象随之泄漏一次；下一次 init 再建 */
}
