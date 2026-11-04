/* apps/demo/app_demo.c —— 样板 App（步骤 3.1）。
 *
 * 这是"一个 App 最短能有多短"的答案，也是后面所有 App 的模板。它故意不碰
 * 任何外设：能被 `host-fake` 跑起来，才说明"App 只依赖 `af_api`"这句话是真的。
 *
 * 三条形态约束都体现在这里：
 *   1. 主任务是 `for (;;) { 干活; fw_delay_block(N); }`，没有状态机、不查返回值；
 *   2. 清理只写在 `exit()` 里——`fw_delay_block()` 之后的代码可能永远不执行；
 *   3. `init()` 只做一次性准备，失败就返回非 `CM_OK`，框架会走 FAILED。
 *
 * 三个计数是**观测口**：写的一方是 App 任务，读的一方是测试/管理器。
 * 跨任务读一个普通 `uint32_t` 在 C11 里就是数据竞争（UB），所以这里一律走
 * `CM_ATOMIC_*` 的 relaxed 档——只保证"读到的是某个完整的旧值或新值"，**不**保证
 * "你看到 ticks 变了就等于看到了任务里的其它写入"。要同步语义请用框架的状态位，
 * 别拿计数器当信号量。
 */
#include "apps/demo/app_demo.h"

#include "common/include/cm_atomic.h"

#define APP_DEMO_ID 2u
#define APP_DEMO_DELAY_MS 100u

/* 三个观测计数器。用途：给测试当"App 真的在跑/真的被回收了"的证据。
 * 谁写：`s_ticks` 由 App 自己的任务每轮 +1；`s_inits`/`s_exits` 由
 *       `app_demo_init()`/`app_demo_exit()` 各 +1（都在 App 任务里）。
 * 并发约定：读的一方通常是测试主任务，所以读写一律走 `CM_ATOMIC_*`（RELAXED）。
 *           它们**不提供同步语义**——别拿计数器当信号量用。 */
static uint32_t s_ticks;
static uint32_t s_inits;
static uint32_t s_exits;

static cm_err_t app_demo_init(app_ctx_t *ctx)
{
    (void)ctx;
    (void)CM_ATOMIC_ADD_FETCH(&s_inits, 1u, CM_MEM_RELAXED);
    return CM_OK;
}

static void app_demo_exit(app_ctx_t *ctx)
{
    (void)ctx;
    (void)CM_ATOMIC_ADD_FETCH(&s_exits, 1u, CM_MEM_RELAXED);
}

static void app_demo_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        (void)CM_ATOMIC_ADD_FETCH(&s_ticks, 1u, CM_MEM_RELAXED);
        fw_delay_block(APP_DEMO_DELAY_MS);
    }
}

const app_desc_t app_demo_desc = {
    APP_DEMO_ID,   "demo",          app_demo_init, app_demo_exit,
    app_demo_task, APP_FLAG_SYSTEM, /* 样板 App 随 boot 起来：它没有管理器，自己就是第一个 */
    APP_STACK_MIN,
};

uint32_t app_demo_ticks(void)
{
    return CM_ATOMIC_LOAD(&s_ticks, CM_MEM_RELAXED);
}

uint32_t app_demo_inits(void)
{
    return CM_ATOMIC_LOAD(&s_inits, CM_MEM_RELAXED);
}

uint32_t app_demo_exits(void)
{
    return CM_ATOMIC_LOAD(&s_exits, CM_MEM_RELAXED);
}

/* 只该在"没有实例在跑"的时候调用（测试的 stop_and_reset 之后）；有实例在跑时
 * 归零不会崩，但那几个数就没有意义了——它是观测口，不是计数器契约。 */
void app_demo_reset_counters(void)
{
    (void)CM_ATOMIC_STORE(&s_ticks, 0u, CM_MEM_RELAXED);
    (void)CM_ATOMIC_STORE(&s_inits, 0u, CM_MEM_RELAXED);
    (void)CM_ATOMIC_STORE(&s_exits, 0u, CM_MEM_RELAXED);
}
