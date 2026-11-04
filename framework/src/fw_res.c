/* framework/src/fw_res.c —— 槽位式资源（步骤 6.1）。
 *
 * 核心只有一句话：**获取函数自己写槽位，App 手上没有裸句柄**。
 *
 * 这一步替掉的是 `res_track(slot, handle)` 那种"先拿到、再登记"的写法。
 * 它不成立的原因在 C 语言本身，不在纪律：
 *
 *     res_track(&ctx->fd, stor_open(path));   —— 看起来是一步，其实不是
 *
 * 参数求值顺序未指定；而 `stor_open()` 一旦在内部命中取消位就会调
 * `fw_run_exit()` —— 那个函数**不返回**。于是 `res_track()` 永远不会执行，
 * 而 fd 已经开出来了。让这种写法成立只有一条路：给每个获取函数加一条
 * "成功之后不得再查取消位"的默契，每个接口各自守，漏一个漏一个。
 *
 * 所以这里反过来：**槽位指针就是资源身份**。`res_acquire()` 在自己体内完成
 * "向平台要 → 登记 → 写槽位"，任何一步失败或取消都能在同一个函数里收尾。
 */
#include "framework/src/fw_internal.h"

/* 槽位指针 → 下标。返回 -1 表示"这个指针不是本实例的槽位"。
 *
 * 这是整个文件里唯一的边界检查：App 传进来的地址必须落在自己的
 * `slots[]` 里，否则它就能往别人的上下文里写句柄。 */
static int slot_index(const fw_app_ctx_t *c, const void *slot)
{
    const char *base = (const char *)&c->pub.slots[0];
    const char *p = (const char *)slot;
    size_t off;

    if (p < base) {
        return -1;
    }
    off = (size_t)(p - base);
    if (off % sizeof(void *) != 0u) {
        return -1;
    }
    off /= sizeof(void *);
    if (off >= APP_CTX_SLOTS) {
        return -1;
    }
    return (int)off;
}

static void slot_free(fw_app_ctx_t *c, int i)
{
    const pf_ops_t *ops = fw_ops();

    if (c->pub.slots[i] == NULL || ops == NULL) {
        return;
    }
    if (c->slot_kind[i] == RES_KIND_MEM) {
        fw_free(c->pub.slots[i]); /* 与 fw_alloc 配对：会同时还配额 */
    }
    /* 槽位是唯一权威：句柄先清掉，`slot_kind` 留成旧值也无害。 */
    c->pub.slots[i] = NULL;
}

static void slot_free_all(fw_app_ctx_t *c, int app_life_only)
{
    uint32_t i;

    for (i = 0; i < APP_CTX_SLOTS; i++) {
        if (c->pub.slots[i] == NULL) {
            continue;
        }
        if (app_life_only && c->slot_life[i] != RES_LIFE_APP) {
            continue; /* 借来的东西：不是我们的，别替他收 */
        }
        slot_free(c, (int)i);
        c->slots_released++;
    }
}

cm_err_t res_acquire(void *slot, res_kind_e kind, size_t bytes, res_life_e life)
{
    const pf_ops_t *ops = fw_ops();
    fw_app_ctx_t *c = fw_current_ctx();
    void *handle = NULL;
    int i;

    if (c == NULL || ops == NULL) {
        return CM_ERR_STATE; /* 不在 App 任务里：没有"当前上下文"就无处登记 */
    }
    if (slot == NULL || bytes == 0u) {
        return CM_ERR_INVALID;
    }
    if (life != RES_LIFE_APP && life != RES_LIFE_BOOT) {
        return CM_ERR_INVALID;
    }
    i = slot_index(c, slot);
    if (i < 0) {
        return CM_ERR_INVALID; /* 不是本 App 的槽位 */
    }
    if (c->pub.slots[i] != NULL) {
        return CM_ERR_BUSY; /* 不覆盖：先 release 再 acquire */
    }

    switch (kind) {
    case RES_KIND_MEM: {
        /* 走记账分配：配额超了要能返回 `CM_ERR_QUOTA`，而不是和"整机没内存"
         * 混成同一个 `CM_ERR_NOMEM`。 */
        cm_err_t rc = CM_OK;

        handle = fw_alloc_ex(c, bytes, &rc);
        if (handle == NULL) {
            return rc; /* 槽位保持空着：失败不留半个登记 */
        }
        break;
    }
    default:
        return CM_ERR_INVALID;
    }

    c->slot_kind[i] = kind;
    c->slot_life[i] = life;
    /* 槽位最后写：在它之前任何一步失败，App 看到的都是"这个槽位是空的"，
     * 不存在"登记了但句柄还没写"的中间态。 */
    c->pub.slots[i] = handle;
    return CM_OK;
}

void res_release(void *slot)
{
    fw_app_ctx_t *c = fw_current_ctx();
    int i;

    if (c == NULL || slot == NULL) {
        return;
    }
    i = slot_index(c, slot);
    if (i < 0) {
        return;
    }
    slot_free(c, i); /* 空槽位时是 no-op：重复调用安全 */
}

void fw_res_release_all(fw_app_ctx_t *c)
{
    if (c == NULL) {
        return;
    }
    slot_free_all(c, 1);
}

uint32_t fw_app_slots_released(const fw_app_ctx_t *c)
{
    return (c == NULL) ? 0u : c->slots_released;
}
