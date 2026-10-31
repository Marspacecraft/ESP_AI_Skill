# 术语表（v1.2）

> 这些词在本仓库里只有一个意思。文档之间打架时，先回这里对齐定义。
> 别在正文里重新定义它们——同一事实写在几处，是文档互相矛盾的主要来源。

## 名字

| 词 | 意思 | 常见误读 |
|---|---|---|
| **ESP AI Skill** | **项目名**，也就是本仓库：一套面向 ESP32 的 App 开发框架 / 开发套件 | 把它当成某个具体产品 |
| **MarsAIReader** | **一个默认硬件平台**：ESP32 上的墨水屏阅读器，用来把"写 App"和"接一块板"两条路径各走通一遍。**它的目录是 `platform/metalio_eink_4_plus/`**——目录名是**板卡型号**，MarsAIReader 是**产品名**，两者不是笔误，也不要为了"看起来一致"去改目录（改目录要动 `policy.txt`，而型号信息本来就不该从路径里消失） | **不是框架的名字**，也不是唯一形态。换板、换一个不是阅读器的 App，框架不改 |
| **管理器** | 负责拉起别的 App、查别人状态、停别人的那个系统 App。属**产品仓库**，不在本仓库 | 把它和"框架"混为一谈。框架只提供机制，管理器才是做决定的那个 |

## 分层与权限

| 词 | 意思 | 常见误读 |
|---|---|---|
| **tag** | **架构分层**编号：`1` 系统 App / `2` 业务 App / `3` 框架 / `4` 平台 / `5` 接口层。它**不是人**——一个人可以同时持多个 tag，同一层可以有多个人 | 把 tag 当用户名。那样第二块板、第二个 AI 出现时编号就撞了 |
| **角色 / 读权 / 写权** | 一个文件归谁写，由 `policy.txt` 里匹配它的**最长前缀**决定；`r` 指"允许 `#include` 该路径的头"。**完整规则不在这里**：见 [roles.md](roles.md) §1 与 [access-control.md](access-control.md) §2 | 以为 `read` 是保密等级；以为 `r` 管链接实现 |

## 编号与身份

| 词 | 意思 | 常见误读 |
|---|---|---|
| **`app_id_t`** | App 的编号。`0` = 无效哨兵（永远不是任何 App）；`1` = 管理器；`2..254` 由 App 作者分配，全固件唯一 | "0 号 App"最容易被当成 `id == 0`。**没有 id 为 0 的 App** |
| **0 号** | 历史叫法，指的是**管理器**，它的 `app_id_t` **是 1** | 见上 |
| **系统 App** | `APP_FLAG_SYSTEM` 的 App：管理器 + 各能力服务（显示 / 输入 / 音频 / 存储 / 网络 / 电源的宿主）。tag 1 | 以为"系统 App"是内核里的东西 |
| **业务 App** | 普通的 App，tag 2。真正对外的那条路径 | 以为它和系统 App 只是"重要程度"不同——实际是**能否 include `framework/include/`** 的区别 |

## 回调与生命周期

| 词 | 意思 |
|---|---|
| **`init` / `exit` / `task`** | App 的三个回调，就这三个名字。**没有 `on_exit`** |
| **伪阻塞** | 一个会阻塞的接口，在内部检查取消位；一旦命中就调 `exit()`，**不返回**。所以"`fw_delay_block()` 之后的代码可能永远不执行"是**正常路径**，不是错误 |
| **取消位** | 每个 App 任务一份的"该停了"标记。**只有 App 任务有**；置位的是**管理器（id 1）**，清零的是任务自己在退出路径上 | 以为框架会去置位（框架没有运行权，置不了） |
| **`STOPPED`** | "即将结束"，**不是**"已结束"。框架没有 join，重启一个刚 `STOPPED` 的 App 可能短暂出现两个任务 |
| **提交型接口** | 调用即返回、最终一致（`disp_commit` 立刻返回，同 key 覆盖）。AI 结构上做不到"刷坏屏" |

## 启动与清单

| 词 | 意思 | 常见误读 |
|---|---|---|
| **`app_main()`** | 本仓库提供的启动入口（`framework/src/fw_main.c`）。ESP32 上它就是 ESP-IDF 的启动入口；host 上由测试或参考平台的 `main()` 调同一个函数。顺序：平台初始化 → `fw_init()` → `fw_boot()` → 拉起管理器 → 管理器拉起其余 App | 以为入口在平台包里 |
| **清单 / `fw_manifest_t`** | "这台设备上有哪些 App"的数组：`{ const app_desc_t *const *apps; uint32_t count; }`。`fw_init(const fw_manifest_t *, uint32_t count)` 可吃 N 份（各作者一份，或入口自己拼成一份）。**没有写进清单的 App 编译好了也不会启动** | 以为能用 `NULL` 结尾当哨兵——`NULL` 是合法的 `app_desc_t *`，会让查重 id 漏掉一个 App |
| **`fw_init()` / `fw_boot()`** | `fw_init()` 合并清单、校验平台两张表版本、连 id 重复一起查；`fw_boot()` 只负责拉起带 `APP_FLAG_SYSTEM` 的 App | 以为 `fw_init()` 会启动 App |

## 代码位置

| 词 | 意思 | 常见误读 |
|---|---|---|
| **`platform/<板>/`** | 每块板的目录，是 `platform/` 的**直接**子目录。**没有脚手架目录**——新板从空目录按契约写 | `platform/boards/<板>/`；或以为有 `platform/template/` 可以 fork |
| **`platform/drivers/`** | 多平台共用的器件驱动（tag 5 写） | 以为它属某一块板 |
| **`api/af_api/`** | App 可见的接口。**v1 只有五个头**：`api.h` / `app.h` / `res.h` / `cap.h` / `log.h`；能力头（`disp`/`input`/`audio`/`stor`/`net`/`power`）还没设计。**`api.h` 不是唯一入口**，只放版本与编译期能力开关 | 以为 App 现在就能 include 显示/网络的头 |
| **`api/fp_api/`** | 框架↔平台之间的契约位，**暂不使用**（有需要再说）。`pf_ops_t` 实际在 `platform/port/pf_ops.h` | 以为平台契约必须搬进 `api/` 才算"接口化" |
| **`common/`** | 公共基础库：类型、错误码、`CM_*` 宏、原子操作。**全层可读**，因为它要跨层共享，且自身不依赖项目内任何头 | 以为它属于某一层；或以为可以往里塞业务逻辑 |
| **`framework/include/`** | 控制面，五个头：`fw_app.h`（状态、取消、退出、启停别人）/ `fw_svc.h`（注册表）/ `fw_event.h`（事件）/ `fw_lock.h`（锁与临界区）/ `fw_init.h`（启动与版本校验）。读权 `1 3 5`，整棵 `framework/` 只有 `3 5` | 以为业务 App 也读得到它 |
| **`platform/port/`** | 平台契约头：`pf_ops.h`（九项能力表）与 `pf_rtos.h`（RTOS 原语表） | —— |
| **`tests/port/`** | 平台一致性套件。**不是普通单测**，是给平台适配者的验收工具 | 以为它是"给框架自己测的" |
| **`tests/baseline/`** | 框架基线测试（tag 5），能直接驱动 `framework/` | —— |
| **`display.md`** | 屏幕的设计与待议问题**同处一份文档**（独立立项、独立版本号），是 md/txt 拆分规则的唯一例外。`api/af_api/disp.h` 在它 §7 的问题定下来之前不存在 | 以为屏幕属于 v1 |

## RTOS 原语

| 词 | 意思 | 常见误读 |
|---|---|---|
| **`pf_rtos_t`** | 框架拿任务、信号量、队列、延时用的**函数指针表**（`platform/port/pf_rtos.h`）。框架**不直接调 RTOS**：ESP32 侧是 FreeRTOS 薄封装 `pf_rtos.c`，host-fake 侧用 pthread | 以为 framework 可以直接 include `freertos/FreeRTOS.h`——纯度禁令不开口子 |
| **NULL 即不支持** | `pf_ops_t` 与 `pf_rtos_t` 用同一种约束：成员为 NULL 就是不支持，由 `fw_init()` 校验返回 `CM_ERR_NOT_SUPPORTED`。**失败点在启动，不在链接** | 以为缺函数会在链接期发现 |

## 错误码

| 词 | 什么时候返回 |
|---|---|
| **`CM_ERR_NOT_SUPPORTED`** | **没这个能力**：平台没实现（`pf_ops_t` 里是 NULL），或服务没注册 |
| **`CM_ERR_UNAVAILABLE`** | **有但坏了**：服务注册了但宿主 App 处于 `FAILED` |
| **`CM_ERR_STATE`** | 状态机不允许这个转换 |
| **`CM_ERR_CANCELLED`** | 伪阻塞接口在等的时候命中取消位。对 `res_lock` 它还保证**未持锁** |
| **`CM_ERR_TIMEOUT`** | 超时。`mutex_take(m, 0)` 必须是"试一次就返回它"，不是永久等待 |

"没这能力"与"有但坏了"必须分开，否则降级路径写错：前者该换实现，后者该重试或提示。

## 平台契约

> 在平台契约定稿之前，各条的完整定义以头文件 `platform/port/pf_ops.h` 与 `pf_rtos.h` 为准；这里只记词面意思。

| 词 | 意思 |
|---|---|
| **九项硬必需** | `mem_alloc` / `mem_free` / `clock_ms` / `sleep_ms` / `mutex_create` / `mutex_take` / `mutex_give` / `task_create` / `task_exit_current`。缺一项 `fw_init()` 直接失败。`log_write` 可选 |
| **`.version`** | `pf_ops_t` 的**首个字段**（不是九项之一），必须 `= PF_PORT_VERSION`。漏填 = 0 = `fw_init()` 第一个检查就失败 |
| **`PF_MUTEX_FOREVER`** | `0xFFFFFFFFu`。传 `0` 是"试一次"，传它是"永久等" |
| **`timeout_ms` 是相对量** | 不是绝对截止时刻。平台自己算 `now + timeout_ms`，用 `(int32_t)(now - deadline) < 0` 处理 49.7 天回绕 |
| **`caps()`** | `pf_ops_t` 的可选成员，返回 `uint32_t` 能力位掩码（9 位已定义，9–31 保留、未定义位必须读出 0）。它只回答"有没有这块硬件"，不回答"接口发布没发布" |
| **host-fake** | 跑在 PC 上的伪平台。它是**验收平台**，不是玩具。它的 `task_exit_current` 是尽力而为，**不能代替真机的 `vTaskDelete(NULL)`** |

## 命名

> 这里只记"有这么几类前缀"。**规则本身在 [coding-style.md](coding-style.md)**（`app_/fw_/pf_/api_/cm_` 层前缀、能力前缀、后缀、类型后缀、snake_case），改命名只该改那一份。

| 词 | 意思 |
|---|---|
| **层前缀** | `app_`（App 自己写的）/ `fw_`（框架实现）/ `pf_`（平台实现）/ `api_`（接口层实现）/ `cm_`（公共库）。按**谁实现**算，不按谁调用 |
| **能力前缀** | `disp_` / `input_` / `audio_` / `stor_` / `net_` / `power_` / `res_` / `cap_`：能力接口实现方不唯一，所以用能力名而不是层名 |

## 两条路径的闭环

| 词 | 意思 |
|---|---|
| **App 侧闭环** | `apps/demo/` + 生命周期测试（六条验收项，见 [app-model.md](app-model.md) §7） |
| **平台侧闭环** | `platform/<板>/` + `tests/port/` 一致性套件。缺了它，"我这块板对不对"只能烧进去试，而九项错误恰恰是烧进去也看不出来的那种 |
