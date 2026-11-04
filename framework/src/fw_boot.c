/* framework/src/fw_boot.c —— 拉起系统 App（步骤 3.2 的框架侧）+ 活实例表。
 *
 * `fw_init()` 只回答"有哪些 App"，`fw_boot()` 才回答"谁先跑"。两者分开是
 * 因为它们的失败语义不同：init 失败 = 整机不启动（清单错了）；boot 失败 =
 * 已经起来的那些还在跑（平台建不出任务，多半是内存或栈配置）。混在一起写，
 * 调用方就分不清"该重试"还是"该改清单"。
 *
 * `fw_boot()` 只拉起 `APP_FLAG_SYSTEM` 的实例；业务 App 由管理器决定，
 * 框架不知道"该起几个"——那是策略，不是机制。
 *
 * 活实例表（D53）：**所有被 `fw_app_start()` 拉起的实例都进表**，不只是
 * `fw_boot()` 起的那些。理由：表如果不能回答"现在都有谁在跑"，管理器一旦丢失
 * 自己保存的指针，就再也枚举不到自己拉起的孩子——"谁负责停它"就成了没人认领的事。
 * 表项一直留到管理器显式 `fw_live_remove()`（D19：先摘表、再释放），所以
 * **已经停掉/崩掉的实例仍在表里**，这是刻意的：管理器要能看到"它停了/它崩了"。
 */
#include "framework/src/fw_internal.h"

/** 活实例表容量。重启会换 id、不复用 id（D18），所以不能按 id 数算满。
 *  用固定上限（与注册表同一个数 253）；真撞上说明重启风暴，该查，不该扩容。 */
#define FW_LIVE_MAX (APP_ID_MAX - APP_ID_MIN + 1u)

/* 槽位式存储：删除只把那一格置 NULL，**不搬动别的格子**。
 *
 * 搬动（整体前移）要写很多格，而管理器随时可能在遍历（`fw_live_count()` +
 * `fw_live_at(i)`），搬动会让读者看到错位或漏项；空洞由 `fw_live_at()` 跳过，
 * 因此遍历顺序仍然是启动顺序。
 *
 * 并发：槽位读写一律 `CM_ATOMIC_*`（指针宽度由元素类型决定）；`s_live_lock` 只护住
 * 两个多步操作——"找空槽"与"定位后置空"——持有时间是 253 次比较，不等外部事件。
 * 锁不覆盖读者的扫描：读者看到的是某一瞬间的快照，这正是 `mgr.h` 承诺的语义。 */
static fw_app_ctx_t *s_live[FW_LIVE_MAX];
static uint32_t s_live_lock;
static int s_booted;

static void fw_live_lock(void)
{
    while (CM_ATOMIC_EXCHANGE(&s_live_lock, 1u, CM_MEM_ACQ_REL) != 0u) {
        /* 管理器路径；临界区只有一次数组扫描。 */
    }
}

static void fw_live_unlock(void)
{
    CM_ATOMIC_STORE(&s_live_lock, 0u, CM_MEM_RELEASE);
}

static fw_app_ctx_t *fw_slot_at(uint32_t i)
{
    return CM_ATOMIC_LOAD(&s_live[i], CM_MEM_ACQUIRE);
}

/** 把一个实例放进活实例表（`fw_app_start()` 建任务之前调）。
 *
 * @param c 已分配好的上下文；不得为 NULL。
 * @return `CM_OK` 已进表；`CM_ERR_NOMEM` 表满（253 个）；`CM_ERR_INVALID` `c` 为 NULL。
 *         线程与中断：管理器/初始化路径；ISR 禁止。
 *         注意事项：任务建失败时调用方必须 `fw_live_remove()` 回滚——表里留一个
 *                   从未启动的实例，管理器会去停一个不存在的东西。
 */
cm_err_t fw_live_add(fw_app_ctx_t *c)
{
    uint32_t i;

    if (c == NULL) {
        return CM_ERR_INVALID;
    }

    fw_live_lock();
    for (i = 0; i < FW_LIVE_MAX; i++) {
        if (fw_slot_at(i) == NULL) {
            CM_ATOMIC_STORE(&s_live[i], c, CM_MEM_RELEASE);
            fw_live_unlock();
            return CM_OK;
        }
    }
    fw_live_unlock();
    return CM_ERR_NOMEM;
}

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
        fw_app_ctx_t *c;
        cm_err_t rc;

        if (d == NULL || (d->flags & APP_FLAG_SYSTEM) == 0u) {
            continue; /* 业务 App 归管理器，框架不替它做决定 */
        }

        c = fw_app_ctx_new(d);
        if (c == NULL) {
            return CM_ERR_NOMEM;
        }
        rc = fw_app_start(c);
        if (rc != CM_OK) {
            fw_app_ctx_free(c); /* start 失败时它已自己摘表，这里只需释放 */
            return rc;
        }
    }
    return CM_OK;
}

uint32_t fw_live_count(void)
{
    uint32_t i;
    uint32_t n = 0u;

    for (i = 0; i < FW_LIVE_MAX; i++) {
        if (fw_slot_at(i) != NULL) {
            n++;
        }
    }
    return n;
}

fw_app_ctx_t *fw_live_at(uint32_t index)
{
    uint32_t i;
    uint32_t seen = 0u;

    for (i = 0; i < FW_LIVE_MAX; i++) {
        fw_app_ctx_t *c = fw_slot_at(i);

        if (c == NULL) {
            continue;
        }
        if (seen == index) {
            return c;
        }
        seen++;
    }
    return NULL;
}

void fw_live_remove(fw_app_ctx_t *c)
{
    uint32_t i;

    if (c == NULL) {
        return;
    }

    fw_live_lock();
    for (i = 0; i < FW_LIVE_MAX; i++) {
        if (fw_slot_at(i) == c) {
            CM_ATOMIC_STORE(&s_live[i], NULL, CM_MEM_RELEASE);
            break;
        }
    }
    fw_live_unlock();
}

void fw_boot_forget(void)
{
    uint32_t i;

    fw_live_lock();
    for (i = 0; i < FW_LIVE_MAX; i++) {
        CM_ATOMIC_STORE(&s_live[i], NULL, CM_MEM_RELEASE);
    }
    fw_live_unlock();
    s_booted = 0;
}
