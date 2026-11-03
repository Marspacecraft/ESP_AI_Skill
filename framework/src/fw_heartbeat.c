/* framework/src/fw_heartbeat.c —— 心跳（步骤 8.1 / 8.2）。
 *
 * 心跳是"我又走完了一段可取消的等待"的事实，不是"我活着"的口号：
 * 一个纯死循环不调任何框架接口，心跳就永远不动——而那种 App 恰恰是这块板上
 * 唯一没法被框架停下来的东西（不检查取消位，取消请求对它无效）。
 *
 * 所以本文件只做两件事：**喂**（框架内部的让出点）和**观测**（给管理器/看门狗）。
 * "发现卡住之后怎么办"不在本仓库——重启整机是产品仓库的决定。
 */
#include "framework/src/fw_internal.h"

void fw_heartbeat_touch(struct fw_app_ctx *c)
{
    if (c == NULL) {
        return;
    }
    (void)__atomic_add_fetch(&c->hb_seq, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&c->hb_ms, fw_ops()->clock_ms(), __ATOMIC_RELEASE);
}

void fw_app_heartbeat(const struct app_ctx *ctx, uint32_t *seq, uint32_t *age_ms)
{
    struct fw_app_ctx *c;
    uint32_t last;
    uint32_t base;

    if (seq != NULL) {
        *seq = 0u;
    }
    if (age_ms != NULL) {
        *age_ms = 0u;
    }
    if (ctx == NULL) {
        return;
    }
    c = (struct fw_app_ctx *)ctx->fw;
    if (c == NULL) {
        return;
    }
    if (seq != NULL) {
        *seq = __atomic_load_n(&c->hb_seq, __ATOMIC_RELAXED);
    }
    last = __atomic_load_n(&c->hb_ms, __ATOMIC_ACQUIRE);
    base = __atomic_load_n(&c->hb_base_ms, __ATOMIC_RELAXED);
    if (age_ms != NULL) {
        *age_ms = (uint32_t)(fw_ops()->clock_ms() - ((last != 0u) ? last : base));
    }
}

int fw_app_is_stalled(const struct app_ctx *ctx, uint32_t stale_ms)
{
    struct fw_app_ctx *c;
    uint32_t last;

    if (ctx == NULL || stale_ms == 0u) {
        return 0;
    }
    c = (struct fw_app_ctx *)ctx->fw;
    if (c == NULL || fw_app_state(c) != FW_APP_STATE_RUNNING) {
        return 0; /* 没在跑就谈不上"卡住"：EMPTY/INIT 还没开始喂 */
    }
    last = __atomic_load_n(&c->hb_ms, __ATOMIC_ACQUIRE);
    if (last == 0u) {
        /* 从没喂过：从进入 RUNNING 算起。纯死循环的 App 就走这条路。 */
        last = __atomic_load_n(&c->hb_base_ms, __ATOMIC_RELAXED);
    }
    return ((uint32_t)(fw_ops()->clock_ms() - last) >= stale_ms) ? 1 : 0;
}
