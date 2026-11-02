/* platform/host-fake/pf_host_fake.h —— host 参考实现的入口。
 *
 * 只有两样东西：一张填好的表，和一个取表的函数。
 * 框架与测试都通过 `const pf_ops_t *` 用它，不直接碰实现细节。
 */
#ifndef ESP_AI_SKILL_PF_HOST_FAKE_H
#define ESP_AI_SKILL_PF_HOST_FAKE_H

#include "platform/port/pf_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/** host 参考实现的端口表（静态常量，进程内唯一）。
 *  入参：无。
 *  返回：永远非 NULL；`version` 恒为 `PF_OPS_VERSION`。
 *  线程与中断：只读常量，任意线程可并发使用；`pf_host_fake_ops_get()` 也不加锁。
 *  注意：它用 pthread + malloc 实现，**只用于 host 测试**；拿它的结果当
 *        真机结论是错的（时序、带宽、中断都不模拟）。 */
extern const pf_ops_t pf_host_fake_ops;

/** 取表指针。
 *  入参：无。
 *  返回：`&pf_host_fake_ops`，永远非 NULL。
 *  线程与中断：任意线程可调；不阻塞。
 *  注意：与上面的全局量是同一张表，二选一即可。 */
const pf_ops_t *pf_host_fake_ops_get(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_PF_HOST_FAKE_H */
