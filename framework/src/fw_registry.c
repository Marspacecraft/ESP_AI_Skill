/* framework/src/fw_registry.c —— 清单合并、查重、注册表（步骤 2.3）。
 *
 * 这一层只做一件事：把"人写的几份清单"变成一个**要么完整、要么为空**的注册表。
 * 没有半份清单——因为半份注册表意味着"启动了，但少了一半 App"，那是最难查的
 * 现场：设备起来了，功能少一个，日志里什么都没说。
 *
 * 所以失败一律回滚到空表，由 `fw_init()` 的返回值一个人承担"整机不启动"。
 */
#include "framework/src/fw_internal.h"

/** 注册表容量：id 取 2..254，正好 253 个，不会出现"id 合法但表满"。 */
#define FW_REG_MAX_APPS (APP_ID_MAX - APP_ID_MIN + 1u)

static const pf_ops_t *s_ops;
static const app_desc_t *s_apps[FW_REG_MAX_APPS];
static uint32_t s_count;
static int s_bound;
static int s_initialized;

/* 已经发出去的 id。**只增不减**：实例退役后 id 也不回收（见 D18）——
 * 复用 id 会让"拿着旧 id 的代码"静默地操作到新实例上，那种 bug 夜里查不出来。 */
static uint32_t s_used_ids[FW_REG_MAX_APPS];
static uint32_t s_used_count;

/** 失败出口：把注册表清回空的初态，再返回错误码。
 *  为什么每个失败点都要过这里：中途 return 会留下"已插入一半"的表，而调用方
 *  只会看到 `CM_ERR_INVALID`，没有人会想到去 `fw_reset()`。 */
static cm_err_t fail_and_clear(cm_err_t rc)
{
    s_count = 0u;
    s_used_count = 0u;
    return rc;
}

const pf_ops_t *fw_ops(void)
{
    /* 原子读：`fw_reset()` 会在测试收尾时把表置空，而此刻可能还有一个任务
     * 在它最后一片 `fw_delay_block()` 里（状态已经是终态、任务还没退出）。
     * 读到 NULL 时调用方会提前返回，所以语义不变；不原子则是一个真实的数据竞争
     * （指针的 8 字节读写没有保证）。 */
    return __atomic_load_n(&s_ops, __ATOMIC_ACQUIRE);
}

cm_err_t fw_platform_bind(const pf_ops_t *ops)
{
    cm_err_t rc;

    if (s_bound || s_initialized) {
        return CM_ERR_STATE; /* 整机只能绑一次；重复绑意味着有人在猜谁先调 */
    }
    rc = fw_ops_check(ops);
    if (rc != CM_OK) {
        return rc; /* 校验失败：s_ops 保持 NULL，不留半绑定 */
    }
    __atomic_store_n(&s_ops, ops, __ATOMIC_RELEASE);
    s_bound = 1;
    return CM_OK;
}

/** 单条目合法性。顺序无所谓，但必须**全部**满足——少一条就是一个"能启动但
 *  行为未定义"的入口。 */
static int app_desc_ok(const app_desc_t *d)
{
    if (d == NULL) {
        return 0;
    }
    if (d->id < APP_ID_MIN || d->id > APP_ID_MAX) {
        return 0; /* 0 = 无效哨兵，1 = 管理器，都不到普通 App 手里来 */
    }
    if (d->name == NULL) {
        return 0; /* 日志里认不出是谁，等于没有名字 */
    }
    if (d->init == NULL || d->exit == NULL || d->task == NULL) {
        return 0;
    }
    if ((d->flags & ~APP_FLAG_SYSTEM) != 0u) {
        return 0; /* 未知标志位：写的人以为它有用，实际上没人为它实现行为 */
    }
    if (d->stack_bytes < APP_STACK_MIN) {
        return 0; /* 启动时拒绝，好过运行期踩坏内存再去猜是哪一层 */
    }
    return 1;
}

cm_err_t fw_init(const fw_manifest_t *manifests, uint32_t count)
{
    uint32_t i;
    uint32_t j;

    if (s_initialized) {
        return CM_ERR_STATE; /* 第二次 init 只会重置一个正在跑的框架 */
    }
    if (!s_bound) {
        return CM_ERR_STATE; /* 没有平台表就没法建任务，先 bind 再 init */
    }
    if (manifests == NULL || count == 0u) {
        return CM_ERR_INVALID;
    }

    s_count = 0u; /* 允许"上次失败后重试"，但表本身从空开始建 */

    for (i = 0; i < count; i++) {
        const fw_manifest_t *m = &manifests[i];

        if (m->count == 0u) {
            continue; /* 空清单合法：某个模块这次没编进来而已 */
        }
        if (m->apps == NULL) {
            return fail_and_clear(CM_ERR_INVALID); /* count>0 却没有数组 */
        }
        for (j = 0; j < m->count; j++) {
            const app_desc_t *d = m->apps[j];
            uint32_t k;

            if (!app_desc_ok(d)) {
                return fail_and_clear(CM_ERR_INVALID);
            }
            for (k = 0; k < s_count; k++) {
                if (s_apps[k]->id == d->id) {
                    /* 重复 id 是最典型的合并事故：两份清单各自"很小心"地避开了
                     * 别人，但没人看全局。表现会是其中一个 App 永远起不来，
                     * 而在设备上看起来只是"这个功能没反应"。 */
                    return fail_and_clear(CM_ERR_INVALID);
                }
            }
            if (s_count >= FW_REG_MAX_APPS) {
                return fail_and_clear(CM_ERR_INVALID); /* 理论不可达，防御式保留 */
            }
            s_apps[s_count++] = d;
        }
    }

    if (s_count == 0u) {
        /* 所有清单都是空的：这不算"启动成功但没有 App"，算清单给错了。 */
        return fail_and_clear(CM_ERR_INVALID);
    }

    /* 清单里的 id 从此归它们所有：后面 `fw_alloc_id()` 不会再发同一个号码。 */
    s_used_count = s_count;
    for (i = 0; i < s_count; i++) {
        s_used_ids[i] = s_apps[i]->id;
    }

    /* 提交表要一把锁（5.2）。放在这里建，而不是第一次提交时惰性建：
     * 惰性建会引入一个"谁先到谁建"的竞争，而 init 本来就是单线程的。 */
    if (fw_submit_init() != CM_OK) {
        return fail_and_clear(CM_ERR_INVALID);
    }

    s_initialized = 1;
    return CM_OK;
}

static int id_used(uint32_t id)
{
    uint32_t i;

    for (i = 0; i < s_used_count; i++) {
        if (s_used_ids[i] == id) {
            return 1;
        }
    }
    return 0;
}

cm_err_t fw_alloc_id(uint32_t *out)
{
    uint32_t id;

    if (out == NULL) {
        return CM_ERR_INVALID;
    }
    if (s_used_count >= FW_REG_MAX_APPS) {
        return CM_ERR_NOMEM; /* 253 个号码发完了：重启次数到顶，该查为什么会一直在重启 */
    }
    for (id = APP_ID_MIN; id <= APP_ID_MAX; id++) {
        if (!id_used(id)) {
            *out = id;
            s_used_ids[s_used_count++] = id;
            return CM_OK;
        }
    }
    return CM_ERR_NOMEM;
}

uint32_t fw_app_count(void)
{
    return s_count;
}

const app_desc_t *fw_app_at(uint32_t index)
{
    if (index >= s_count) {
        return NULL;
    }
    return s_apps[index];
}

void fw_reset(void)
{
    uint32_t i;

    for (i = 0; i < s_count; i++) {
        s_apps[i] = NULL;
    }
    s_count = 0u;
    s_used_count = 0u;
    __atomic_store_n(&s_ops, (const pf_ops_t *)NULL, __ATOMIC_RELEASE);
    s_bound = 0;
    s_initialized = 0;
    fw_svc_forget();
    fw_submit_forget();
    fw_event_forget();
    fw_boot_forget();
}
