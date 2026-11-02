/* common/include/cm_types.h —— 公共层的基础类型与错误码。
 *
 * 为什么在 common/ 而不是 api/：`api/` 的两支（af_api 给 App、fp_api 给框架↔平台）
 * 都要用错误码；放进任何一支，另一支就得反向依赖它。common/ 两边都能 include。
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
 *   - `CM_ERR_CANCELED`：App 正在被停止，调用点不该再继续；通常由框架内部用，
 *     极少回给 App（App 收到它就该顺着退出路径走）。
 */
typedef enum {
    CM_OK = 0,               /**< 成功。所有 API 的零值都是它，便于 `if (rc)` 判定 */
    CM_ERR_INVALID = 1,      /**< 参数非法（NULL、越界、状态不对） */
    CM_ERR_NOMEM = 2,        /**< 内存不足 */
    CM_ERR_NOT_SUPPORTED = 3,/**< 这块板没有这个能力（合法状态，不是故障） */
    CM_ERR_UNAVAILABLE = 4,  /**< 能力在，但当前不可用（服务 FAILED / 未就绪） */
    CM_ERR_VERSION = 5,      /**< 契约版本不匹配 */
    CM_ERR_TIMEOUT = 6,      /**< 等待超时 */
    CM_ERR_BUSY = 7,         /**< 同一 key 已有未完成的提交 */
    CM_ERR_STATE = 8,        /**< 状态机上不允许这个动作（如对 STOPPED 之外的再 start） */
    CM_ERR_CANCELED = 9      /**< 当前 App 正在被停止 */
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
