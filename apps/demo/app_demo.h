/* apps/demo/app_demo.h —— 样板 App 的描述体与三个观察口。
 *
 * 这个 App 的全部价值在于"它长什么样"，而不是"它做了什么"：一个没有屏幕、
 * 没有文件、没有网络的 App，能证明的只有生命周期本身——init 跑了没有、
 * 主任务转了几圈、exit 是不是恰好一次。
 *
 * 所以它对外只暴露三样东西：描述体、转了几圈、以及两个计数器。测试靠它们
 * 判断生命周期，而不是靠日志。
 */
#ifndef ESP_AI_SKILL_APP_DEMO_H
#define ESP_AI_SKILL_APP_DEMO_H

#include "api/af_api/app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 样板 App 的描述体。`APP_FLAG_SYSTEM` 让 `fw_boot()` 直接拉起它。 */
extern const app_desc_t app_demo_desc;

/** 主任务转过多少圈（每圈一次 `fw_delay_block()`）。 */
uint32_t app_demo_ticks(void);

/** `init()` 被调用过几次。样板 App 只跑一次，这个数用于验证"没有偷偷重启"。 */
uint32_t app_demo_inits(void);

/** `exit()` 被调用过几次。契约是 1（含失败路径）。 */
uint32_t app_demo_exits(void);

/** 清零三个计数器（测试用；真机上没有第二次启动）。 */
void app_demo_reset_counters(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_APP_DEMO_H */
