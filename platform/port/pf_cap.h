/* platform/port/pf_cap.h —— 能力契约（**反向**：平台交给框架）。
 *
 * 为什么需要这一份，而不是把能力塞进 `pf_ops.h`：
 *
 *   `pf_ops_t` 收的是**机制**——没有它框架就跑不起来（内存、时钟、睡眠、锁、
 *   任务）。判据只有一句：**去掉它，框架还能不能跑？** 显示、存储、网络、音频、
 *   按键都不满足这一条，所以它们不进那张表（D56）。
 *
 *   于是产生一个反方向的洞：板子有屏，但框架没有任何形式能拿到"怎么刷一帧"。
 *   `pf_ops.h` 的头注释说"能力差异只用 caps() 与服务注册表表达"，而服务 App
 *   的 link 集合不含 `platform/`（App 拿不到驱动）、平台又拿不到 `svc.h`
 *   （注册不进服务表）——两头都够不着中间。本头就是这个中间（D57）。
 *
 * 板子怎么用（一行）：
 *
 *     fw_cap_register(PF_CAP_DISPLAY, &my_disp_ops);      // 在 boot 里，fw_init 之前
 *
 *   **不实现就不调**：没屏的板不需要写空实现、不需要 `#if`、不需要占位符号。
 *
 * 没注册会怎样：`cap_available(PF_CAP_DISPLAY)` 回答 false，能力调用统一返回
 * `CM_ERR_NOT_SUPPORTED`。注意这与"硬件有没有"是两件事——`caps()` 说"查得到这块
 * 硬件"，注册表说"软件能调它"。两者都有才算可用（见 `api/af_api/cap.h` 的注释）。
 *
 * 三条使用规则（违反任何一条，平台与框架就不在一个契约上）：
 *   1. 本头**自包含**：只 include 系统头与本目录的 `pf_ops.h`，不 include
 *      `common/`、`framework/`、`api/`。
 *   2. `fw_cap_register()` 虽然带 `fw_` 前缀（它由框架实现），但它**只允许板子
 *      在 boot 阶段调用一次/每位**，不允许 App 调用。App 看见的是
 *      `api/af_api/cap.h`。
 *   3. 注册的指针必须**活到关机**：框架只存指针，不做拷贝，也不负责释放。
 */
#ifndef ESP_AI_SKILL_PF_CAP_H
#define ESP_AI_SKILL_PF_CAP_H

#include <stddef.h>
#include <stdint.h>

#include "pf_ops.h" /* PF_CAP_* 位号：能力位的唯一出处 */

#ifdef __cplusplus
extern "C" {
#endif

/** 当前定义的能力位个数（`PF_CAP_DISPLAY`..`PF_CAP_RTC` 共九位）。
 *  新增能力位时这个数要一起改，`tools/check_caps.py` 会核对目录。 */
#define PF_CAP_COUNT 10u /* 位 0..9，见 pf_ops.h 的 PF_CAP_* */

/** 把一项能力的实现表交给框架。
 *
 * @param bit 能力位，必须是 `PF_CAP_*` 中**单独一位**（0、多位组合、保留位都不接受）。
 * @param ops 指向板子自己的实现表（例如 `pf_disp_ops_t`）。不得为 NULL；
 *            该对象必须活到关机，框架只存指针不拷贝。
 * @return 0 注册成功；-1 位号非法、`ops` 为 NULL、或这一位**已经注册过**
 *         （重复注册一律拒绝：两份实现同时存在，谁生效取决于调用顺序，
 *         这种"看谁先跑"的语义比没有实现更难查）。
 *         线程与中断：只在 boot 阶段（`fw_init()` 之前）调用；不得在 ISR 里调用。
 *         注意事项：框架不负责释放 `ops`；撤销注册只有框架内部的
 *         `fw_cap_forget()`（测试收尾用），板子没有"注销"这条路。
 */
int fw_cap_register(uint32_t bit, const void *ops);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_PF_CAP_H */
