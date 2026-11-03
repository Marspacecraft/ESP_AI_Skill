/* framework/src/fw_svc.c —— 服务注册表（步骤 5.1）。
 *
 * 表很小、语义很硬：**注册的是"谁提供"，不是"能力存在"**。能力存在与否由
 * `caps()` 说（阶段 1 的裁决），这里说的是"这个产品上有没有 App 在提供它"。
 * 两者分开，是因为 `PF_CAP_STORAGE` 可以是 1，而这个固件里根本没编存储服务。
 *
 * 并发：三个查询/写入都可能来自不同任务（App A 查、App B 注册）。没有用锁——
 * 每条表项就是一个指针 + 一个 id，`__atomic` 的 CAS 足够，且不会在 ISR 里
 * 制造"取锁超时"这种第二类失败。
 *
 * 槽位复用：`id` 决定槽位（`slot = id % FW_SVC_MAX`）会让 64 与 0 撞车；这里
 * 用**线性查找**，最多 64 次比较，比哈希诚实得多，也不会因为"两个 id 落一个槽"
 * 造出一类只在特定 id 组合下才出现的 bug。
 */
#include "framework/src/fw_internal.h"

static fw_svc_id_t s_svc_id[FW_SVC_MAX];
static struct fw_app_ctx *s_svc_owner[FW_SVC_MAX];

static int id_valid(fw_svc_id_t id)
{
    return id != 0u && (uint32_t)id < FW_SVC_MAX;
}

static int find_slot(fw_svc_id_t id)
{
    uint32_t i;

    for (i = 0; i < FW_SVC_MAX; i++) {
        if (__atomic_load_n(&s_svc_id[i], __ATOMIC_ACQUIRE) == id) {
            return (int)i;
        }
    }
    return -1;
}

static int free_slot(void)
{
    uint32_t i;

    for (i = 0; i < FW_SVC_MAX; i++) {
        if (__atomic_load_n(&s_svc_id[i], __ATOMIC_ACQUIRE) == 0u) {
            return (int)i;
        }
    }
    return -1;
}

cm_err_t fw_svc_register_ctx(fw_svc_id_t id, struct fw_app_ctx *owner)
{
    int slot;

    if (!id_valid(id) || owner == NULL) {
        return CM_ERR_INVALID;
    }
    slot = find_slot(id);
    if (slot >= 0) {
        if (s_svc_owner[slot] == NULL) {
            /* 表项还在、owner 已经死了（上次崩过 / 重启中）：**认领**它。
             * 这正是"重试"能成功的那条路。 */
            __atomic_store_n(&s_svc_owner[slot], owner, __ATOMIC_RELAXED);
            return CM_OK;
        }
        return (s_svc_owner[slot] == owner) ? CM_ERR_STATE : CM_ERR_BUSY;
    }
    slot = free_slot();
    if (slot < 0) {
        return CM_ERR_NOMEM; /* 表满：这是产品配置问题，不是暂时性失败 */
    }
    __atomic_store_n(&s_svc_owner[slot], owner, __ATOMIC_RELAXED);
    /* id 最后写：读的一方先看到 owner 非空、再看到 id，顺序反过来会读到
     * "id 有了但 owner 还是 NULL"的半成品表项。 */
    __atomic_store_n(&s_svc_id[slot], id, __ATOMIC_RELEASE);
    return CM_OK;
}

cm_err_t fw_svc_unregister_ctx(fw_svc_id_t id, struct fw_app_ctx *owner)
{
    int slot = find_slot(id);

    if (!id_valid(id)) {
        return CM_ERR_INVALID;
    }
    if (slot < 0) {
        return CM_ERR_NOT_SUPPORTED;
    }
    if (owner == NULL ||
        __atomic_load_n(&s_svc_owner[slot], __ATOMIC_ACQUIRE) != owner) {
        return CM_ERR_STATE;
    }
    __atomic_store_n(&s_svc_id[slot], (fw_svc_id_t)0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_svc_owner[slot], NULL, __ATOMIC_RELAXED);
    return CM_OK;
}

struct fw_app_ctx *fw_svc_owner(fw_svc_id_t id)
{
    int slot;

    if (!id_valid(id)) {
        return NULL;
    }
    slot = find_slot(id);
    return (slot < 0) ? NULL
                      : __atomic_load_n(&s_svc_owner[slot], __ATOMIC_ACQUIRE);
}

cm_err_t fw_svc_ready(fw_svc_id_t id)
{
    struct fw_app_ctx *owner;

    if (!id_valid(id)) {
        return CM_ERR_INVALID;
    }
    if (find_slot(id) < 0) {
        return CM_ERR_NOT_SUPPORTED;   /* 从来没人注册：这块板/这个产品没这能力 */
    }
    owner = fw_svc_owner(id);
    if (owner == NULL) {
        return CM_ERR_UNAVAILABLE;     /* 注册过，但提供者已经没了：可以重试 */
    }
    /* 只有"正在跑"才算可用：INIT 还没准备好，STOPPING/STOPPED/FAILED 都不能用。 */
    if (fw_app_state(owner) != FW_APP_STATE_RUNNING) {
        return CM_ERR_UNAVAILABLE;
    }
    return CM_OK;
}

void fw_svc_drop_owner(struct fw_app_ctx *owner)
{
    uint32_t i;

    if (owner == NULL) {
        return;
    }
    /* **只清 owner 指针，不清 id**：表项留着才能让 `fw_svc_ready()` 回答
     * "曾经提供过、现在没了"（UNAVAILABLE，可以重试），而不是"没这能力"
     * （NOT_SUPPORTED，重试一万次也没用）。两者混成一个码，调用方就只剩放弃。
     * 上下文即将被管理器释放，所以 owner 指针必须清掉——留着就是野指针。 */
    for (i = 0; i < FW_SVC_MAX; i++) {
        if (__atomic_load_n(&s_svc_owner[i], __ATOMIC_ACQUIRE) == owner) {
            __atomic_store_n(&s_svc_owner[i], NULL, __ATOMIC_RELAXED);
        }
    }
}

void fw_svc_forget(void)
{
    uint32_t i;

    for (i = 0; i < FW_SVC_MAX; i++) {
        /* 与注册/摘除用同一套原子写：混用普通写会让另一个任务读到"半个表项"，
         * 也让 TSan 把改前的 7 类竞争修完之后又报出一条新的。 */
        __atomic_store_n(&s_svc_id[i], (fw_svc_id_t)0u, __ATOMIC_RELEASE);
        s_svc_owner[i] = NULL;
    }
}

/* ── 公开面（D44）：owner 一律取"当前 App" ─────────────────────────────
 *
 * 内部实现要显式传 owner，是因为测试与框架自己需要在别人的上下文里注册/注销
 * （例如验证"别人的 owner 抢不走我的表项"）。App 永远只为自己注册，所以公开面
 * 不给 owner 参数——少一个能被传错的东西。
 */
cm_err_t fw_svc_register(fw_svc_id_t id)
{
    struct fw_app_ctx *c = fw_current_ctx();

    if (c == NULL) {
        return CM_ERR_STATE; /* 不在 App 任务里：没有"当前 App"可注册 */
    }
    return fw_svc_register_ctx(id, c);
}

cm_err_t fw_svc_unregister(fw_svc_id_t id)
{
    struct fw_app_ctx *c = fw_current_ctx();

    if (c == NULL) {
        return CM_ERR_STATE;
    }
    return fw_svc_unregister_ctx(id, c);
}
