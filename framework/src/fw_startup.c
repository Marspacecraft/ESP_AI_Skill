/* framework/src/fw_startup.c —— 启动序列（步骤 4.1）。
 *
 * 三种宿主共用这一段：整机固件（`fw_main.c` 的 `app_main()`）、嵌入方
 * （`fw_embed_start()`）、以及测试。所以它**不能**和 `app_main()` 住在同一个
 * 编译单元里——`ai-passport` 那次真机链接失败
 * （`undefined reference to 'fw_startup'`）就是因为它俩捆在一起：
 * 组件必须排除 `fw_main.c`（否则和产品自己的 `app_main` 撞车），
 * 一排除就把序列也一起排掉了。
 *
 * 顺序不能换：bind 先校验版本与九项完整性，后面的 init/boot 都假设表可用。
 */
#include "framework/src/fw_internal.h"
#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

cm_err_t fw_startup(const pf_ops_t *ops, const fw_manifest_t *man)
{
    cm_err_t err;

    if (ops == NULL || man == NULL) {
        return CM_ERR_INVALID;
    }

    /* 顺序不能换：bind 先校验版本与九项完整性，后面的 init/boot 都假设表可用。 */
    err = fw_platform_bind(ops);
    if (err != CM_OK) {
        return err;
    }

    /* 板子在这里注册能力实现（`fw_cap_register`）。放在 bind 之后、init 之前：
     * 之后框架已经能回答 `cap_available()`，而第一个 App 还没起来——
     * 反过来（init 之后）就有"App 已经看到能力不存在"的窗口。 */
    pf_platform_boot();

    err = fw_init(man, 1u);
    if (err != CM_OK) {
        return err;
    }
    return fw_boot();
}
