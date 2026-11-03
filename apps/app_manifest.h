/* apps/app_manifest.h —— 本仓库自带的 App 注册清单。
 *
 * "没写进清单的 App 编译好了也不会启动"——清单是启动的唯一入口，
 * 所以它只有一份，且**只许追加**（tag 1 与 tag 2 共写，见 policy.txt）。
 *
 * 每一份清单是一个独立数组 + 一个 `fw_manifest_t`：产品仓库可以带自己的清单，
 * `fw_init()` 收 N 份。用数组 + count 而不是 NULL 结尾，是因为 NULL 本身是
 * 合法的 `app_desc_t*`，靠它收尾会让查重时漏掉最后一个。
 */
#ifndef ESP_AI_SKILL_APP_MANIFEST_H
#define ESP_AI_SKILL_APP_MANIFEST_H

#include "framework/include/fw_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 本仓库自带的清单（目前只有样板 App）。 */
extern const fw_manifest_t apps_manifest;

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_APP_MANIFEST_H */
