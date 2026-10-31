# App 模型（v1.1）

> **状态：定稿 v1.1（2026-10-30）**。改动按 `docs/README.md` 规则 9：提理由 + diff → 批准 → 落盘 → 升次版本号。
> v1.1 只做了一件事：把同名 `app-model.txt` 整份并入 §9（缺口与待定），设计条文一字未改。

**它回答什么问题**：一个 App 由什么构成？谁让它运行、谁让它停止？它凭什么能写直线代码？

> §1–§8 是设计结果；**§9 是缺口与待定**（还没做的、还没定的），别当规范读。

对应实现：`api/af_api/app.h`（App 可见）、`framework/include/fw_app.h`（控制面，只给系统 App / 框架 / 接口层；**没有 `api/sys/`**）、`framework/src/fw_app.c` / `fw_task.c` / `fw_main.c`（`app_main()` 在这里）。

**本仓库只提供最小 App 集**：`apps/demo/`（样板 App）与 `apps/app_manifest.c`（清单），它们是 AI 的写入目标、也是生命周期测试的驱动对象。系统 App 与服务宿主（`apps/system/`）由产品仓库提供。框架只负责合并清单、拉起 `APP_FLAG_SYSTEM` 的 App。

> 规则 8：接口的权威定义是头文件。本文只说约束、理由和失败模式。

---

## 1. 一个 App = 一个描述体 + 三个函数

```c
typedef struct app_desc {
    app_id_t id;                       /* 2..254 由 AI 分配；0 = 无效哨兵，1 固化给管理器 */
    const char *name;                  /* 给人看的名字：日志、UI */
    cm_err_t (*init)(app_ctx_t *ctx);  /* 在 App 自己的任务里跑 */
    void     (*exit)(app_ctx_t *ctx);  /* 任务结束前一定调用一次 */
    void     (*task)(app_ctx_t *ctx);  /* 主任务体 */
    uint32_t flags;
} app_desc_t;
```

**为什么 id 是整数（`app_id_t`，底层 `cm_u8`）而不是字符串**：它要出现在注册表查找、将来的按 App 配额与位图里，
每次比较走 `strcmp` 是白花时间；位图又让"一共有哪些 App"变成一个 32 位整数。
代价是 id 必须手工分配且唯一——`fw_init()` 会检查重复并拒绝启动，因为重复 id 的表现是
"有时启动的是另一个 App"，属于最难查的一类 bug。人读的名字留在 `name`，日志一律打 name。

三条设计取舍：

1. **没有"创建"和"销毁"回调，只有 init/exit**。区别在于：exit 在**取消路径上也会被调用**，
   所以它必须能处理"init 只做了一半"的状态。多一个 destroy 只会让作者以为可以只在正常路径清理。
2. **没有 `start` 参数、没有返回值传递**。App 不给别人返回值——它通过服务注册表暴露能力，
   通过状态位暴露死活。这样"启动失败"只有一种表现：`FAILED`。
3. **`ctx` 是不透明的**。App 不能读它的字段，因此框架可以自由改内部布局（现在里面只有
   cancel 与 state 两个原子量，将来会加资源登记表）。

## 2. 生命周期

```
EMPTY --start--> INIT --init成功--> RUNNING --request_stop--> STOPPING --> STOPPED
                   |                   |                                    |
                   +--init失败--> FAILED +--task返回（自愿退出）------------+
                                                                          |
                                                STOPPED --start--> INIT   （可重启）
```

规则：

- `EMPTY` / `STOPPED` 是**唯一**允许启动的两个状态，其他状态 `fw_app_start()` 返回 `CM_ERR_STATE`。
- `exit()` 在**任何**结束路径上都会被调用恰好一次，包括 `FAILED`。
- `FAILED` 不自动重启。单 MCU 上"重启 App"最多是"重建它的任务"，失败循环的代价
  （电量、日志、可能的硬件半初始化状态）比失败本身更大。重启策略属于管理器（id 1）的策略层。
- `SYSTEM` 标志只决定"谁先被启动"（`fw_boot()` 直接拉起），**不是特权等级**：
  带这个标志的 App 自己也能被停止。

## 3. 伪阻塞契约（整个模型的地基）

App 写直线代码：

```c
static void hello_task(app_ctx_t *ctx)
{
    for (;;) {
        do_work();
        fw_delay_block(200);   /* 被停止时这里直接退出任务，不返回 */
    }
}
```

- 框架提供的**每一个**会阻塞的接口都是伪阻塞的：内部按切片等待，每片检查取消位，
  发现取消就调用内部的退出函数（`fw_run_exit()`，见 §6 的机制面）——**不返回**。
- 因此 App **不需要**检查返回值、不需要写状态机、不需要轮询取消位。
- 因此 `fw_delay_block()` **之后**的代码不一定会执行 → 清理只能写在 `exit()` 里，
  不能写在循环后面。

退出流程做五件事，顺序不能变：

1. 清零取消位（= 在 `exit()` 期间禁用取消，否则清理被反复打断，永远结束不了）；
2. 调用 `desc->exit(ctx)`；
3. 回收框架托管过的资源（`res_track` 登记过的槽位）；
4. 置终态（`STOPPED` / `FAILED`）；
5. 结束当前任务，不返回。

**为什么不是"检查返回值"模型**：那样每个调用点都要写 `if (r == CM_ERR_CANCELLED) return;`，
漏一个就变成"停止请求发了但 App 还在跑"。伪阻塞把这件事收敛到框架一处，
App 作者想漏也漏不掉。

**面积代价**：App 一旦进入框架接口就再也回不到"我要自己收尾"的位置，所以
`exit()` 必须能看到并清理它需要的一切。当前的做法是"App 自己记住自己开了什么"；
资源自动登记（`res_track`）的接口见 [architecture.md](architecture.md) §5。

## 4. 谁启动谁

```
app_main()                         ← 本仓库提供的唯一入口（framework/src/fw_main.c）
  ├── 平台初始化                    ← 填 pf_ops_t 与 pf_rtos_t 两张表
  ├── fw_init(manifests, count)     ← 校验平台版本 + 合并 N 份清单进注册表（查重 id）
  └── fw_boot()                     ← 拉起带 APP_FLAG_SYSTEM 的 App
        └── 系统 App 的任务体（通常就是管理器）
              └── 自己决定启动哪些业务 App：fw_app_count() / fw_app_at()
                    └── fw_delay_block(100) + 观测状态变化
```

**框架只做一件启动决定**：`APP_FLAG_SYSTEM` 的 App 由 `fw_boot()` 直接拉起。
其余的由谁、按什么顺序拉起，是**系统 App 自己的策略**——框架不规定，也不认识
具体是谁。管理器（id 1；文档里常叫"0 号"）就是"实现了这个策略的那个 App"，它不在本仓库里。

`fw_init()` 与 `fw_boot()` 的分工不能混：前者只做**校验与合并**（平台两张表的版本、清单数量、id 重复），后者只做**拉起**。启动失败因此只有两种：`fw_init()` 返回非 `CM_OK`（一个 App 都没起步），或者某个 App 自己进 `FAILED`。

启动顺序因此是**可读的**，不需要猜：框架按清单数组顺序拉起 `APP_FLAG_SYSTEM` 的
App；业务 App 的顺序由系统 App 自己决定，写在它的代码里，同样不是散落各处。

## 5. 清单由调用方提供，可以有多份

```c
typedef struct {
    const app_desc_t *const *apps;
    uint32_t count;
} fw_manifest_t;

cm_err_t fw_init(const fw_manifest_t *manifests, uint32_t count);
```

**为什么是"清单数组 + 数量"，而不是一个全局数组**：不同 App 的作者维护各自的
清单，彼此读不到对方的目录。框架把 N 份清单合并进内部的注册表（`FW_APP_MAX`
上界），合并只发生在框架里。

这个形状是**权限判定的符号判定逼出来的**。最初的实现只有一张
`apps/app_manifest.c`，AI 加自己的 App 时必须写
`extern const app_desc_t g_app_manager;` 才能把它放进启动列表——于是 AI 的
编译单元引用了系统 App 的符号。判定器第一次跑就报：

```
SYM  apps/app_manifest.c (tag 2) 用了 g_app_manager，但没有任何实际 include 的头声明它
```

改成**清单按作者拆分**以后问题消失：AI 的清单里只出现自己的 App，系统 App 的清单由别人维护，框架负责合并。具体到路径（与 `policy.txt` 一致）：

| 清单 | 谁写 / 谁读 | 内容 |
|---|---|---|
| `apps/app_manifest.c` / `.h` | **写**：tag 1 与 tag 2（共写）　**读**：tag 1、2、3 | AI 的业务 App 清单。AI 加 App = 往数组末尾**追加一行** |
| `apps/system/system_manifest.c` | tag 1（系统 App 作者） | 系统 App 与服务宿主（含管理器） |

**只能追加，不能重写。** 清单是两个 tag 共写的文件，允许整份重写等于任一方都能改坏对方的条目（删掉别人的行，或把自己的 App 改写成 `APP_FLAG_SYSTEM` 绕过管理器的启动策略）。这条规则还没有机器判定，现在靠 review 拦。

**为什么不按 App 拆成每人一份文件**：`policy.txt` 给 tag 2 的写路径只有三处（`apps/demo/`、`apps/app_manifest.c`、`.h`），没有开放的 `apps/<app>/manifest.c`。所以按**作者组**拆、不按 App 拆。这也让"AI 加一个 App"成为一个可门禁的动作：一张数组末尾多一行。

> 这里是判定器抓出来的结构缺陷，不是设计时想到的：最早的单一 `apps/app_manifest.c` 逼 AI 写 `extern const app_desc_t g_app_manager;`，符号判定直接报 `SYM apps/app_manifest.c (tag 2) 用了 g_app_manager，但没有任何实际 include 的头声明它`。

### 本仓库提供的最小 App 集

| 在本仓库 | 内容 |
|---|---|
| `apps/demo/` | 样板 App：`init` / `exit` / `task` + 自己的 `<名字>.h`（声明 `extern const app_desc_t g_app_demo;`） |
| `apps/app_manifest.c` / `.h` | AI 的清单；`.h` 声明描述体符号，`.c` 是那张表 |

系统 App 与服务宿主（`apps/system/`）**不在本仓库**——它们由产品仓库提供，其中包含管理器的启动策略。

加一个 App 的完整步骤见 `skills/role-app/SKILL.md`。

## 6. 头文件按"谁会用"切开

| 头 | 谁能 include | 能给什么 |
|---|---|---|
| `api/af_api/` | 所有 App（tag 1、2） | 写 App 需要的一切：id、名字、`fw_delay_block`、`fw_alloc`、`fw_log`、资源与能力查询。**v1 只有五个头**（`api.h`/`app.h`/`res.h`/`cap.h`/`log.h`），能力头还没设计 |
| `api/fp_api/` | 框架（3）、平台（4） | 框架↔平台之间的契约位，**暂不使用**。`pf_ops_t` 实际在 `platform/port/pf_ops.h`（"`pf_ops.h` 放哪"这一议题已关闭，见 [architecture.md](architecture.md) §10） |
| `common/include/` | 所有层 | 类型、错误码、`CM_*` 宏 |
| `framework/include/` | 系统 App（1）、框架（3）、测试（5） | App 状态、`fw_cancelled()`、`fw_run_exit()`、启动/停止别人。五个头：`fw_app.h` / `fw_svc.h` / `fw_event.h` / `fw_lock.h` / `fw_init.h` |

**没有 `api/sys/` 了。** 机制面（`fw_run_exit` / `fw_cancelled` / 状态查询）回到 `framework/include/`：它本来就只有系统 App 与测试能用，硬塞进 `api/` 会让"`api/` 对所有 App 开放"这条规矩破例。代价是系统 App 要 include 框架的头——这是**唯一的跨层例外**，`policy.txt` 里用 `read framework/include/ 1 3 5` 显式开的口子（整棵 `framework/` 只给 `3 5`）。

为什么把"机制面"单独拆出来，而不是全塞进 `api/af_api/app.h`：

- `fw_run_exit()` 能让当前 App 立刻结束。对 AI 可见，它就会变成**逃生门**——
  "这个错误我处理不了，让整个 App 重启吧"。这类代码在墨水瓶上表现为反复重启、反复刷屏。
- `fw_cancelled()` 一旦可见，每个 App 都会开始写 `if (fw_cancelled()) break;`，
  于是"伪阻塞由框架一处保证"退化成"N 个作者各自记得做"。这正是取消机制不该暴露的原因。

**注意"伪阻塞"这件事本身仍然是公开契约**（在 `api/af_api/app.h` 里写了）：AI 必须知道
"`fw_delay_block()` 之后的代码不一定执行"、"清理只能写在 `exit()` 里"。看不见的是**怎么实现的**，
不是**行为是什么**。

机制面的声明放 `framework/include/`（`fw_app.h`）而不是 `apps/_system/`：实现在 `framework/src/`，
声明若放进应用层目录，框架就得反向 include 应用层——依赖方向会翻过来。

同一个 `api/` 函数有两个实现家（机制型在 `framework/src/`，服务型在服务 App + 服务注册表），
但**调用方看不出区别**——见 code-structure.md §4.1。

## 7. 验收

**v1 的验收线是六条生命周期断言，全部在 host 上，全部要跑出数字**（真机不在 v1 验收线内）：

| # | 断言 | 判据 |
|---|---|---|
| 1 | 停止时延 | 一个卡在 `fw_delay_block(200)` 的 App，从置取消位到 `STOPPED` **< 500ms** |
| 2 | `exit()` 恰好一次 | 计数 0 → 1 → 重启 → 2 |
| 3 | 重启 | `STOPPED` 之后能再 `init`，状态回 `RUNNING` |
| 4 | 非法转换 | 返回 `CM_ERR_STATE`，不改状态 |
| 5 | SYSTEM 也能被停 | 不是"系统 App 停不下来" |
| 6 | `init` 失败仍调一次 `exit()` | 失败路径也走清理 |

**怎么造被测对象**（不依赖任何产品 App）：在测试文件里用本地 `app_desc_t` 造两个假 App（一个 SYSTEM、一个普通），走真实的 `fw_init` / `fw_boot` / `fw_app_start` / `fw_app_stop`。

## 8. 决定记录

> **本节编号是 App 模型内部的局部编号**，不与其它文档共享，也不跨文件复用。本节保留的是 App 模型内部的推理过程，只追加不删除。

| # | 决定 | 理由 | 状态 |
|---|---|---|---|
| 1 | App = 模块 + 主任务 + 描述体，不是独立进程 | 单 MCU 没有地址空间隔离，进程模型是假的 | 定稿 |
| 2 | 只留 init / exit / task 三个回调 | exit 覆盖所有结束路径，避免作者只在正常路径清理 | 定稿 |
| 3 | 阻塞接口全部伪阻塞，App 不检查返回值 | 把"停不下来"的失败收敛到框架一处 | 定稿 |
| 4 | `EXIT` 顺序：清零取消位 → exit() → 回收托管资源 → 置终态 → 结束任务 | 顺序错会导致"清理被打断"或"重启立刻又退出" | 定稿 |
| 5 | `FAILED` 不自动重启 | 单 MCU 上"重启 App"最多是"重建它的任务"，失败循环的代价（电量、日志、可能的硬件半初始化状态）比失败本身更大。框架只把 App 置为 `FAILED` 并让位；要不要重试是管理器看见 `FAILED` 之后的事，不是 App 的自我重启 | 定稿 |
| 6 | 只允许从 `EMPTY`/`STOPPED` 启动 | 防止重入与双任务；其余状态返回 `CM_ERR_STATE` | 定稿 |
| 7 | `SYSTEM` = 启动顺序，不是特权 | 管理器也会被停；权限不该靠一个 flag 隐式表达 | 定稿 |
| 11 | 清单由调用方提供，可以有多份（`fw_manifest_t` + count） | 一张全局表会让 AI 的注册单元引用系统 App 的 desc 符号（越权 + 脆弱）。这条是权限判定的符号判定抓出来的，不是设计时想到的 | 定稿 |
| 14 | id：`0` = 无效哨兵，`1` = 管理器，`2..254` 归 AI | 早期文档混用"0 号 manager"与"0 保留"，实际不兼容 | 定稿 |

> 编号 **8、9、10、12、13** 不在本表：8 已被 11 取代；9、12、13 的落点在 §9（分别对应平台契约、最小 App 集、清单按作者组）；10（栈统一 4096、暂不按 App 配置）还没定案。

## 9. 缺口与待定

> 本节是**当前状态**，不是设计结果。它回答"这套模型里哪些还没做、哪些还没定"，实现时不要把它当规范读。

### 9.1 停止语义的边界

`STOPPED` 是"**即将结束**"而不是"已经结束"。平台没有 join 语义，所以观测到 `STOPPED` 后立刻 `start`，理论上能出现两个任务。当前用"轮询间隔 + 重启前让出一次"把它压在毫秒级。要彻底消除需要 port 增加 `task_join`，代价是 ESP32 侧各 RTOS 变体与 host-fake 的**退出语义不一致**（FreeRTOS 是"任务自删"，POSIX 是 `pthread_exit`，main 线程还特殊）。

### 9.2 没有任务级隔离

"强制回收"最多是"标记 → 停止投递 → 重建任务 → 不行就重启整机"。**不能让 AI 以为这里有 Linux 级的隔离。**

### 9.3 `res_track` 未实现

所以 §3 退出流程的第 3 步是**空转**：App 必须自己记住开了哪些资源并在 `exit()` 里关掉。漏了的后果是句柄泄漏，而不是"框架帮你收"。

### 9.4 栈大小固定 4096（未定案）

所有 App 一样。按 App 配置栈是下一步，先统一是为了避免"某个 App 栈溢出"变成随机崩溃。

### 9.5 没有配额、没有看门狗

这两者是**管理器（id 1）**的策略，而管理器不在本仓库。框架只保证机制（启动、停止、状态查询），不代替策略。

### 9.6 纯死循环框架发现不了

伪阻塞只在"App 调用框架接口"时生效；`while (1) { work(); }` 不调用任何接口时，取消位永远不会被看到。静态门禁（`check_blocking.py`）与心跳看门狗**都没实现**，所以目前只能重启整机。详见 [architecture.md](architecture.md) §3.1.5。

### 9.7 六条验收还没有数字

`tests/`、`apps/` 都不存在，所以 §7 的六条一条都没跑过——"没有失败"是因为没运行。**六条在 host 上没跑出数字之前，任何文档不许写"生命周期已验证"。**

### 9.8 这一版还没跟上的三处

- §3 退出流程第 3 步（回收 `res_track` 登记过的资源）目前什么也不做，见 §9.3。
- §4 的"平台初始化"只有一句话：填 `pf_ops_t` / `pf_rtos_t` 两张表 + 校验端口版本，具体检查哪些项没写（也记在 [code-structure.txt](code-structure.txt)）。
- §5 的清单**"只许追加"没有机器判定**，现在靠 review 拦（判定清单见 [code-structure.txt](code-structure.txt)）。

### 9.9 从决定表移出的三行（不是作废，是原位在别处）

| 原编号 | 决策 | 现在的位置 |
|---|---|---|
| 9 | 不用 stub 函数表示"不支持"，用 NULL | 平台契约 —— 函数指针为 NULL 即"没有这能力"，见 `policy.txt` 之外的头文件定义（平台契约文档待重写） |
| 12 | 本仓库只提供最小 App 集（`apps/demo/` + `apps/app_manifest.c` / `.h`） | [architecture.md](architecture.md) §0 与 `policy.txt` |
| 13 | 清单按**作者组**拆，不按 App 拆 | [code-structure.md](code-structure.md) §7 与本文 §5 |

### 9.10 已作废的决定（保留编号以免后面的决定串号）

| # | 原决定 | 结局 |
|---|---|---|
| 8 | 注册点集中在 `apps/app_manifest.c` 一张表 | **已被 11 取代**：清单由调用方提供、可以有多份 |

