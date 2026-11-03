/* framework/src/fw_ctx.c —— App 上下文与任务入口（步骤 2.4 的一半）。
 *
 * 这一份只回答两个问题：**上下文长什么样、任务怎么起来**。退出路径在
 * `fw_exit.c`，切片等待在 `fw_delay.c`——它们要能互相调用，所以分开写，
 * 但共享 `struct fw_app_ctx`（见 fw_internal.h）。
 *
 * 为什么任务入口要自己设 `_Thread_local`：`fw_delay_block()` 没有 ctx 参数
 * （App 的写法是 `fw_delay_block(100)`），框架要回答"现在是谁在睡"，只有两条路——
 * 要么给平台表加一个"取当前任务"的接口，要么用语言自带的线程局部存储。
 * 选后者：平台九项是给接板的人看的契约，不该为框架的内部记账多长一条（D15）。
 */
#include <string.h>

#include "framework/src/fw_internal.h"

/** 当前任务的上下文。**每个任务一份**，由 `fw_app_entry()` 设置。
 *  主线程、平台任务、ISR 里都是 NULL——那里的 `fw_delay_block()` 退化成
 *  "纯切片睡眠"，没有取消位可查。 */
static _Thread_local struct fw_app_ctx *t_current;

/** 任务入口：init → RUNNING → task。
 *
 * 三条结束路径都汇到 `fw_run_exit()`，所以 `exit()` 只会被调用一次，
 * 且失败路径（init 失败、task 返回）也走同一条路——"恰好一次"不是靠三处
 * 各自记得调用维持的。
 */
static void fw_app_entry(void *arg)
{
    struct fw_app_ctx *c = (struct fw_app_ctx *)arg;

    t_current = c;

    CM_ATOMIC_STORE(&c->state, (uint32_t)FW_APP_STATE_INIT, CM_MEM_RELEASE);
    if (c->desc->init(&c->pub) != CM_OK) {
        fw_run_exit(c, FW_APP_STATE_FAILED);
    }

    /* 心跳基准：从来没喂过心跳的 App（纯死循环）也要能算出"多久没动了"。 */
    CM_ATOMIC_STORE(&c->hb_base_ms, fw_ops()->clock_ms(), CM_MEM_RELAXED);
    CM_ATOMIC_STORE(&c->state, (uint32_t)FW_APP_STATE_RUNNING, CM_MEM_RELEASE);
    c->desc->task(&c->pub);

    /* 走到这里说明 task() 返回了。按契约，主任务是不该返回的——
     * 返回等于"我自己认为我挂了"，所以是 FAILED 而不是 STOPPED。 */
    fw_run_exit(c, FW_APP_STATE_FAILED);
}

struct fw_app_ctx *fw_app_ctx_new_with_id(const app_desc_t *desc, uint32_t id)
{
    const pf_ops_t *ops = fw_ops();
    struct fw_app_ctx *c;

    if (ops == NULL || desc == NULL || ops->mem_alloc == NULL) {
        return NULL;
    }
    if (id < APP_ID_MIN || id > APP_ID_MAX) {
        return NULL; /* 0 是无效哨兵，1 是管理器，都不该跑到实例 id 上 */
    }
    /* 清单路径由 `fw_init()` 校验过；这里直接建的实例没经过注册表，
     * 必须自己校验一遍——否则 NULL 的 init/task 会在任务入口里变成段错误，
     * 而段错误是这块板上最难查的那种失败。 */
    if (desc->name == NULL || desc->init == NULL || desc->exit == NULL ||
        desc->task == NULL) {
        return NULL;
    }

    c = (struct fw_app_ctx *)ops->mem_alloc(sizeof *c);
    if (c == NULL) {
        return NULL;
    }

    (void)memset(c, 0, sizeof *c);
    c->desc = desc;
    c->pub.id = id;
    c->pub.fw = c;
    CM_ATOMIC_STORE(&c->state, (uint32_t)FW_APP_STATE_EMPTY, CM_MEM_RELEASE);
    return c;
}

struct fw_app_ctx *fw_app_ctx_new(const app_desc_t *desc)
{
    if (desc == NULL || desc->id < APP_ID_MIN || desc->id > APP_ID_MAX) {
        return NULL;
    }
    return fw_app_ctx_new_with_id(desc, desc->id);
}

void fw_app_ctx_free(struct fw_app_ctx *c)
{
    const pf_ops_t *ops = fw_ops();

    if (c == NULL || ops == NULL || ops->mem_free == NULL) {
        return;
    }
    ops->mem_free(c);
}

cm_err_t fw_app_start(struct fw_app_ctx *c)
{
    const pf_ops_t *ops = fw_ops();
    pf_task_desc_t td;

    if (c == NULL || ops == NULL || ops->task_create == NULL) {
        return CM_ERR_STATE;
    }
    if (CM_ATOMIC_LOAD(&c->state, CM_MEM_ACQUIRE) != (uint32_t)FW_APP_STATE_EMPTY) {
        return CM_ERR_STATE;
    }

    td.name = c->desc->name;
    td.entry = fw_app_entry;
    td.arg = c;
    td.stack_bytes = c->desc->stack_bytes;

    /* 先把状态推离 EMPTY：任务一建好就可能立刻跑起来，
     * 不这么做的话"同一上下文启动两次"的窗口是开着的。 */
    CM_ATOMIC_STORE(&c->state, (uint32_t)FW_APP_STATE_INIT, CM_MEM_RELEASE);

    if (ops->task_create(&td) == NULL) {
        CM_ATOMIC_STORE(&c->state, (uint32_t)FW_APP_STATE_EMPTY, CM_MEM_RELEASE);
        return CM_ERR_NOMEM;
    }
    return CM_OK;
}

fw_app_state_e fw_app_state(const struct fw_app_ctx *c)
{
    if (c == NULL) {
        return FW_APP_STATE_EMPTY;
    }
    return (fw_app_state_e)CM_ATOMIC_LOAD(&c->state, CM_MEM_ACQUIRE);
}

uint32_t fw_app_exit_calls(const struct fw_app_ctx *c)
{
    return (c == NULL) ? 0u : CM_ATOMIC_LOAD(&c->exit_calls, CM_MEM_ACQUIRE);
}

void fw_app_request_cancel(struct fw_app_ctx *c)
{
    if (c != NULL) {
        CM_ATOMIC_STORE(&c->cancel, 1u, CM_MEM_RELEASE);
    }
}

struct fw_app_ctx *fw_current_ctx(void)
{
    return t_current;
}
