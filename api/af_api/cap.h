/* api/af_api/cap.h —— 能力查询（App 可见的契约）。
 *
 * `cap_available()` 回答的是"**这一样现在能调吗**"，由两件事合起来判：
 *
 *   1. 平台报了这一位——`caps()` 说"硬件在"；
 *   2. 板子通过 `fw_cap_register()` 交了实现——`platform/port/pf_cap.h`。
 *
 * 只有 (1) 就宣称"能调"，会把"查得到硬件"和"调得动它"混成一件事（`pf_ops.h`
 * 头注释专门警告过这种假信号）；接口头有没有发布是第三件事，看
 * `docs/generated/catalog.md` 的「接口头」一列。
 *
 * 所以"能不能用"要问两层：`cap_available()` 为真 + 对应的能力头已发布。
 *
 * 依赖约束：只能 include `common/`（与 `app.h`/`res.h`/`evt.h`/`svc.h` 相同）。
 * 平台表 `pf_ops_t` 对 App **不可见**（`policy.txt` 的 link 行不给 tag 1/2 读
 * `platform/`），所以能力位不能直接从 `PF_CAP_*` 拿——这里按位号重新声明，
 * 位号一致性由 `tests/platform/test_stack_contract.c` 的 `_Static_assert` 钉住：
 * 任何一边改了位号，编译不过。
 */
#ifndef ESP_AI_SKILL_AF_API_CAP_H
#define ESP_AI_SKILL_AF_API_CAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 位 0–9：与 `platform/port/pf_ops.h` 的 `PF_CAP_*` 逐位对应（位号不能改）。
 * 位 10–31 保留，平台必须读 0，`fw_ops_check()` 在启动时就拒掉违例的表。 */
#define CAP_DISPLAY (1u << 0u)         /* 有屏 */
#define CAP_DISPLAY_PARTIAL (1u << 1u) /* 屏支持局部刷新（没有不代表不能刷，只代表要靠全刷） */
#define CAP_TOUCH (1u << 2u)           /* 触摸 */
#define CAP_KEY (1u << 3u)             /* 按键 */
#define CAP_STORAGE (1u << 4u)         /* 掉电不丢的存储 */
#define CAP_NET (1u << 5u)             /* 网络 */
#define CAP_AUDIO_OUT (1u << 6u)       /* 扬声器 */
#define CAP_AUDIO_IN (1u << 7u)        /* 麦克风 */
#define CAP_RTC (1u << 8u)             /* 实时时钟（能跨断电计时） */
#define CAP_UART (1u << 9u)            /* 至少一路可用的应用串口（接口头 `uart.h`） */

/**
 * 查某个能力位这块板有没有。
 *
 * @param bit 单个能力位，取 `CAP_*` 之一。传 0、传多位组合、或传保留位（9–31）
 *            一律返回 false——**这不算错误**，调用方本来就该按"没有"处理。
 * @return 该位为 1 **且**板子注册了该位的实现 → true；其余（表没绑、位为 0、
 *         多于一 bit、保留位、平台没报这一位、报了但没注册实现）→ false。
 *
 * 线程与中断：任意任务可调；不阻塞、不取锁、不分配内存。
 *             ISR 里**不要**调——能力位是启动期就定死的，中断里没有"临时发现没有"这回事。
 * 注意：返回 true 说明"硬件在 **且** 软件实现已交给框架"。对应的接口头没发布时，
 *       你依然调不到它；具体调什么见 `docs/generated/catalog.md`。
 */
bool cap_available(uint32_t bit);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_CAP_H */
