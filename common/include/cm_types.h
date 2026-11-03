/* common/include/cm_types.h —— 公共层的基础类型与错误码。
 *
 * 为什么在 common/ 而不是 api/：App 门面与平台契约都要用错误码；放进任何
 * 一支，另一支就得反向依赖它（而依赖方向只能是 apps → framework → platform）。
 * common/ 是唯一"谁都能 include、谁也不反向依赖"的位置。
 *
 * 规则：错误码只增不改，数值一旦发布就是契约的一部分（日志、测试、未来跨仓库
 * 的返回值比对都靠它）。新增一律取下一个没用过的值，不复用、不重排。
 */
#ifndef ESP_AI_SKILL_CM_TYPES_H
#define ESP_AI_SKILL_CM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 全仓统一错误码。
 *
 * 分段含义（选哪个码本身就是设计决定，不要随手挑一个能编译的）：
 *   - `CM_ERR_INVALID` / `CM_ERR_NOMEM`：参数或资源不对，是自己的错；
 *   - `CM_ERR_NOT_SUPPORTED`：**这块板没有这个能力**，不是 bug，不该重试；
 *   - `CM_ERR_UNAVAILABLE`：能力在，但当前用不了（服务崩了/还没起来），可以重试；
 *   - `CM_ERR_VERSION`：契约版本不匹配，启动期就该拒，不要试图兼容；
 *   - `CM_ERR_CANCELED`：**预留值，当前不会被任何函数返回**（见下）。取消的
 *     语义是"不再返回"而不是"返回一个码"：取消位命中时框架直接走退出路径，
 *     调用点根本执行不到。清理只能写在 `exit()` 里，不要写成"检查返回值"。
 *     保留 9 这个号是为了将来真有"早退"场景时不必占新号；在那之前，谁写
 *     `if (rc == CM_ERR_CANCELED)` 谁就是在写死代码。
 *   - `CM_ERR_QUOTA`：**你**用超了配额，不是整机没内存。两者分开的理由：
 *     前者该查自己是不是漏了释放，后者该查是不是真该省着用；合成一个码，
 *     现场就只能靠猜。
 */
typedef enum {
    CM_OK = 0,                /**< 成功。所有 API 的零值都是它，便于 `if (rc)` 判定 */
    CM_ERR_INVALID = 1,       /**< 参数非法（NULL、越界、状态不对） */
    CM_ERR_NOMEM = 2,         /**< 内存不足 */
    CM_ERR_NOT_SUPPORTED = 3, /**< 这块板没有这个能力（合法状态，不是故障） */
    CM_ERR_UNAVAILABLE = 4,   /**< 能力在，但当前不可用（服务 FAILED / 未就绪） */
    CM_ERR_VERSION = 5,       /**< 契约版本不匹配 */
    CM_ERR_TIMEOUT = 6,       /**< 等待超时 */
    CM_ERR_BUSY = 7,          /**< 同一 key 已有未完成的提交 */
    CM_ERR_STATE = 8,         /**< 状态机上不允许这个动作（如对 STOPPED 之外的再 start） */
    CM_ERR_CANCELED = 9,      /**< 预留：**当前无产出者**，不要用它做分支 */
    CM_ERR_QUOTA = 10         /**< 超出了本 App 的资源配额（不是"系统没内存"） */
} cm_err_t;

/** 错误码的稳定短名，只用于日志与测试输出。
 *
 * @param e 任意 `cm_err_t`（非法值不崩溃，返回 "CM_ERR_?"）。
 * @return 静态字符串，永不 NULL，调用方不得释放。
 *         线程安全：只读常量表，任意线程可调。
 *         注意：返回值是给人看的，**不要拿它做逻辑判断**——要判就判数值。
 */
const char *cm_err_str(cm_err_t e);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_CM_TYPES_H */
