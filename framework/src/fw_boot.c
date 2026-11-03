/* framework/src/fw_boot.c —— 拉起系统 App（步骤 3.2 的框架侧）。
 *
 * `fw_init()` 只回答"有哪些 App"，`fw_boot()` 才回答"谁先跑"。两者分开是
 * 因为它们的失败语义不同：init 失败 = 整机不启动（清单错了）；boot 失败 =
 * 已经起来的那些还在跑（平台建不出任务，多半是内存或栈配置）。混在一起写，
 * 调用方就分不清"该重试"还是"该改清单"。
 *
 * 这一版只拉起 `APP_FLAG_SYSTEM` 的：按约定通常是管理器（id 1 的活，但管理器
 * 自己的 id 由产品仓库定）。业务 App 由管理器决定——框架不知道"该起几个"，
 * 因为那是策略，不是机制。
 */
#include "framework/src/fw_internal.h"

/** 活实例表。容量与注册表一致：同时活着的实例不可能多于注册的 App 数……
 *  但**重启会换 id 不复用 id**，所以这里不能按 id 算容量。用固定上限，
 *  和注册表同一个数（253）；真撞上说明重启风暴，该查，不该扩容。 */
#define FW_LIVE_MAX (APP_ID_MAX - APP_ID_MIN + 1u)

static struct fw_app_ctx *s_live[FW_LIVE_MAX];
static uint32_t s_live_count;
static int s_booted;

cm_err_t fw_boot(void)
{
    uint32_t i;
    uint32_t n;

    if (s_booted) {
        return CM_ERR_STATE; /* 第二次 boot 会把同一批 App 起两遍 */
    }

    n = fw_app_count();
    if (n == 0u) {
        return CM_ERR_STATE; /* 没 init 过，或者 init 后表是空的 */
    }

    s_booted = 1;
    for (i = 0; i < n; i++) {
        const app_desc_t *d = fw_app_at(i);
        struct fw_app_ctx *c;
        cm_err_t rc;

        if (d == NULL || (d->flags & APP_FLAG_SYSTEM) == 0u) {
            continue; /* 业务 App 归管理器，框架不替它做决定 */
        }
        if (s_live_count >= FW_LIVE_MAX) {
            return CM_ERR_NOMEM;
        }

        c = fw_app_ctx_new(d);
        if (c == NULL) {
            return CM_ERR_NOMEM;
        }
        rc = fw_app_start(c);
        if (rc != CM_OK) {
            fw_app_ctx_free(c);
            return rc;
        }
        s_live[s_live_count++] = c;
    }
    return CM_OK;
}

uint32_t fw_live_count(void)
{
    return s_live_count;
}

struct fw_app_ctx *fw_live_at(uint32_t index)
{
    if (index >= s_live_count) {
        return NULL;
    }
    return s_live[index];
}

void fw_live_remove(struct fw_app_ctx *c)
{
    uint32_t i;

    if (c == NULL) {
        return;
    }
    for (i = 0; i < s_live_count; i++) {
        if (s_live[i] == c) {
            /* 后面整体前移：表的顺序是启动顺序，管理器按顺序遍历它，
             * 留空洞会让遍历提前结束。 */
            s_live_count--;
            for (; i < s_live_count; i++) {
                s_live[i] = s_live[i + 1u];
            }
            s_live[s_live_count] = NULL;
            return;
        }
    }
}

void fw_boot_forget(void)
{
    uint32_t i;

    for (i = 0; i < s_live_count; i++) {
        s_live[i] = NULL;
    }
    s_live_count = 0u;
    s_booted = 0;
}
