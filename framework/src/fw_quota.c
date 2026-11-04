/* framework/src/fw_quota.c —— 堆内存记账与配额（步骤 6.2）。
 *
 * 这里实现两件事，它们必须住在同一个文件里：
 *   ① `fw_alloc` / `fw_free` —— 每个分配前面挂一个头，记住"多大的、谁的"；
 *   ② 配额 —— 用这个头把字节数记到 owner 头上。
 *
 * 为什么必须有头：`fw_free(void *p)` 拿不到长度，而记账需要"还多少"。让调用方
 * 再传一次长度是把同一个事实写两遍，且写错的后果是记账慢慢漂移（越漂越松，
 * 最后配额形同虚设）。头是唯一能自己回答"这块多大"的地方。
 *
 * 为什么头里记 owner 而不记"当前任务"：释放可能发生在别的任务里（管理器收
 * 别人的垃圾），那时候 `fw_current_ctx()` 不是块的主人。记在头里，谁释放都
 * 还到对的账上。
 *
 * 记账是**原子**的，而且先加后判：两个任务同时申请时，两边都先把自己的字节数
 * 加上去，谁把总额顶过配额谁退回来。先判后加会有一个窗口，让两个放行之后
 * 总额超限——配额就变成了"大致配额"。
 */
#include "framework/src/fw_internal.h"

#define FW_ALLOC_MAGIC 0x414C4C4Fu /* "ALLO" */

/** 每个分配前面的头。**用户拿到的指针是它之后的那一段**。
 *
 * 字段顺序刻意让 `size` 与 `owner` 都自然对齐；`pad` 把整个头撑到指针宽度的
 * 整数倍（64 位上是 24 字节，32 位上是 16 字节），用户指针因此总是对齐的。
 */
struct fw_alloc_hdr {
    size_t size;         /* 用户要的字节数（不含头） */
    fw_app_ctx_t *owner; /* 记到谁的账上；可空（框架自己分配） */
    uint32_t magic;      /* 校验：不是我们发的指针一律不碰 */
    uint32_t pad;
};

int fw_quota_reserve(fw_app_ctx_t *c, size_t bytes)
{
    size_t now;
    size_t quota;

    if (c == NULL) {
        return 1; /* 不在 App 里分配（框架内部）：不计账，也不受配额限制 */
    }
    quota = CM_ATOMIC_LOAD(&c->quota_bytes, CM_MEM_RELAXED);
    now = CM_ATOMIC_ADD_FETCH(&c->used_bytes, bytes, CM_MEM_RELAXED);
    if (quota != 0u && now > quota) {
        (void)CM_ATOMIC_SUB_FETCH(&c->used_bytes, bytes, CM_MEM_RELAXED);
        (void)CM_ATOMIC_ADD_FETCH(&c->denied_count, 1u, CM_MEM_RELAXED);
        return 0;
    }
    return 1;
}

void fw_quota_release(fw_app_ctx_t *c, size_t bytes)
{
    if (c == NULL) {
        return;
    }
    (void)CM_ATOMIC_SUB_FETCH(&c->used_bytes, bytes, CM_MEM_RELAXED);
}

static void *alloc_impl(fw_app_ctx_t *c, size_t bytes, cm_err_t *out)
{
    const pf_ops_t *ops = fw_ops();
    struct fw_alloc_hdr *hdr;

    if (out != NULL) {
        *out = CM_ERR_INVALID;
    }
    if (ops == NULL || bytes == 0u) {
        return NULL; /* 0 字节不是"成功分配一个空块"，是参数错误 */
    }
    if (!fw_quota_reserve(c, bytes)) {
        if (out != NULL) {
            *out = CM_ERR_QUOTA;
        }
        return NULL;
    }
    hdr = (struct fw_alloc_hdr *)ops->mem_alloc(sizeof *hdr + bytes);
    if (hdr == NULL) {
        fw_quota_release(c, bytes);
        if (out != NULL) {
            *out = CM_ERR_NOMEM;
        }
        return NULL;
    }
    hdr->size = bytes;
    hdr->owner = c;
    hdr->magic = FW_ALLOC_MAGIC;
    hdr->pad = 0u;
    if (out != NULL) {
        *out = CM_OK;
    }
    return (void *)((unsigned char *)hdr + sizeof *hdr);
}

void *fw_alloc(size_t bytes)
{
    return alloc_impl(fw_current_ctx(), bytes, NULL);
}

/** 内部版本：要错误码的地方（`res_acquire`）用这个，区分 QUOTA 与 NOMEM。 */
void *fw_alloc_ex(fw_app_ctx_t *c, size_t bytes, cm_err_t *out)
{
    return alloc_impl(c, bytes, out);
}

void fw_free(void *p)
{
    const pf_ops_t *ops = fw_ops();
    struct fw_alloc_hdr *hdr;

    if (p == NULL || ops == NULL) {
        return; /* NULL 无操作：调用方不用先判空 */
    }
    hdr = (struct fw_alloc_hdr *)((unsigned char *)p - sizeof *hdr);
    if (hdr->magic != FW_ALLOC_MAGIC) {
        /* 不是 `fw_alloc` 发出去的指针。**不动它**：把别人的内存塞进我们的
         * 分配器，只会把一个错误变成两个。这一块会泄漏，但堆还是好的。 */
        return;
    }
    hdr->magic = 0u; /* 先失效，重复释放时上面那道检查会拦住 */
    fw_quota_release(hdr->owner, hdr->size);
    ops->mem_free(hdr);
}

cm_err_t fw_res_set_quota(struct app_ctx *ctx, size_t bytes)
{
    fw_app_ctx_t *c = (fw_app_ctx_t *)ctx->fw;

    if (ctx == NULL || c == NULL) {
        return CM_ERR_INVALID;
    }
    CM_ATOMIC_STORE(&c->quota_bytes, bytes, CM_MEM_RELEASE);
    return CM_OK;
}

cm_err_t fw_res_revoke(struct app_ctx *ctx)
{
    fw_app_ctx_t *c = (fw_app_ctx_t *)ctx->fw;
    size_t used;

    if (ctx == NULL || c == NULL) {
        return CM_ERR_INVALID;
    }
    used = CM_ATOMIC_LOAD(&c->used_bytes, CM_MEM_RELAXED);
    if (used == 0u) {
        used = 1u; /* 压到 0 会变成"不限"，那是反的 */
    }
    CM_ATOMIC_STORE(&c->quota_bytes, used, CM_MEM_RELEASE);
    return CM_OK;
}

void fw_res_usage(const struct app_ctx *ctx, size_t *used_bytes, size_t *quota_bytes,
                  uint32_t *denied_count)
{
    const fw_app_ctx_t *c = (ctx == NULL) ? NULL : (const fw_app_ctx_t *)ctx->fw;

    if (used_bytes != NULL) {
        *used_bytes = (c == NULL) ? 0u : CM_ATOMIC_LOAD(&c->used_bytes, CM_MEM_RELAXED);
    }
    if (quota_bytes != NULL) {
        *quota_bytes = (c == NULL) ? 0u : CM_ATOMIC_LOAD(&c->quota_bytes, CM_MEM_RELAXED);
    }
    if (denied_count != NULL) {
        *denied_count = (c == NULL) ? 0u : CM_ATOMIC_LOAD(&c->denied_count, CM_MEM_RELAXED);
    }
}
