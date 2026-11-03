/* api/af_api/app.h —— App 的入口头（App 只 include 这一个就能写完整）。
 *
 * 它只做一件事：include `framework/include/fw_app.h`，把 `app_desc_t`、三个
 * 回调类型、`struct app_ctx`、以及 `fw_delay_block` / `fw_alloc` / `fw_free` /
 * `fw_log` / `fw_logp` 带进来。
 *
 * 为什么回调签名和 `app_ctx` 都不在这里定义：同一事实两个家，早晚漂移（D11）。
 * 布局搬去 `fw_app.h` 的理由见 D17——框架要实现 `app_ctx`，而框架不能 include
 * `api/`（那是 App 的包）。
 *
 * 本头的价值不在"多定义了什么"，而在**名字**：App 的词汇表叫 `af_api`，
 * 框架的词汇表叫 `fw_`，两者不混，App 永远不需要知道后者在哪。
 */
#ifndef ESP_AI_SKILL_AF_API_APP_H
#define ESP_AI_SKILL_AF_API_APP_H

#include "framework/include/fw_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 这里没有 `struct app_ctx` 了：布局已经搬进 `framework/include/fw_app.h`（D17）。
 * 原因很实在——框架要往里写状态、取消位、任务句柄，而框架不能 include `api/`。
 * 一个只有 App 看得见、实现者看不见的结构体，是没法被实现的。
 *
 * 本头现在的全部内容是：include `fw_app.h`。它的价值不在"多定义了什么"，
 * 而在**名字**：App 的词汇表叫 `af_api`，框架的词汇表叫 `fw_`，两者不混。
 */

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_APP_H */
