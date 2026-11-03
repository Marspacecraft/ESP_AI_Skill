/* framework/include/fw_app.h —— 框架侧的转发头（D44）。
 *
 * **契约不在这里。** App 的词汇表（`app_desc_t` / 回调 / `app_ctx` / 使用面 /
 * 资源 / 事件 / 服务）全部在 `api/af_api/` 下：
 *
 *   api/af_api/app.h   生命周期 + 使用面（`fw_delay_block` / `fw_alloc` / `fw_log` / 心跳）
 *   api/af_api/res.h   资源槽位 + 配额
 *   api/af_api/evt.h   事件
 *   api/af_api/svc.h   服务注册表 + 提交型接口
 *
 * 本头只做一件事：把 `api/af_api/app.h` 带进来。存在的原因只有两个：
 *   1. 框架内部源码历史上 include 的是本头，改一行就够，不必动 30 个文件；
 *   2. `policy.txt` 里 tag 3 的 link 集合同时含 `api/af_api/` 与 `framework/include/`，
 *      两条路径都合法。
 *
 * 别再往这里加声明。加了就会回到"同一件事两个家"，`tools/check_link.py` 会把闭包
 * 摊开给你看（D31），评审时也看得见。
 *
 * 历史：D11 说"本头是唯一出处、api/af_api/app.h 只 include 本头"；D17 因为
 * "框架要写 `app_ctx`、而框架 include 不了 `api/`"把整套搬到这里。D44 推翻了
 * D17 的前提——`link 1 2 3 api/af_api/` 一直允许框架读 `api/`，那句话从未成立。
 */
#ifndef ESP_AI_SKILL_FW_APP_H
#define ESP_AI_SKILL_FW_APP_H

#include "api/af_api/app.h"

#endif /* ESP_AI_SKILL_FW_APP_H */
