/* tests/app/surface_compile_check.c —— 2.2 的验收：只 include app.h 能不能写出完整 App。
 *
 * 故意**不**取名 test_*.c：它调用 fw_delay_block / fw_logp，而这两个的实现要等 2.4。
 * 现在链接必然失败，所以只做编译检查；等 2.4 落地后改名 test_app_surface.c，
 * 让它进 ./build.sh run 的正式回路。
 */
#include "api/af_api/app.h"

static cm_err_t demo_init(app_ctx_t *ctx)
{
    ctx->slots[0] = NULL; /* 阶段 6 之前只是布局预留 */
    return CM_OK;
}

static void demo_exit(app_ctx_t *ctx)
{
    (void)ctx;
}

static void demo_task(app_ctx_t *ctx)
{
    for (uint32_t tick = 0;; tick++) {
        fw_logp("demo", "tick=%u id=%u", (unsigned)tick, (unsigned)ctx->id);
        fw_delay_block(100);
    }
}

static const app_desc_t demo_desc = {
    2u, "demo", demo_init, demo_exit, demo_task, 0u
};

static const app_desc_t *const demo_apps[] = { &demo_desc };
static const fw_manifest_t demo_manifest = { demo_apps, 1u };

int main(void)
{
    /* 只用类型与常量，不调用任何使用面函数——真正的调用在上面那个任务体里，
     * 编译期能看见就够了。 */
    return (int)(demo_manifest.count + demo_desc.id);
}
