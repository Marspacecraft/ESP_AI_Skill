# 角色与权限（v1.0）

> **状态：定稿 v1.0（2026-10-30）**

> 这份文件回答三个问题：**谁是谁**、**谁能写什么**、**谁必须停下来问**。
> 机器可读的事实只有一处：仓库根的 `policy.txt`。本文是它的人话版本 + 理由；**冲突时以 `policy.txt` 为准**。
> 判定机制（怎么判"你越界了"）见 [access-control.md](access-control.md)。

---

## 1. tag 是架构分层，不是人

一个人可以持多个 tag（角色 B 同时持 2 和 3）；同一层也可以有多个人。

把 tag 当成"用户名"，是这套模型最容易犯的错——那样在**第二块板**、**第二个 AI** 出现时就会撞。

| 层            | tag | 名字          | 角色  | 能写（与 `policy.txt` 的 `role` 行一致）                                                                                     | 能读（与 `policy.txt` 的 `read` 行一致，同一单元格里的多个前缀按最长匹配解读）                                                                                                               |
| ------------ | --- | ----------- | --- | ------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 应用层 · 系统 App | 1   | `sysapp`    | A   | `apps/system/` `apps/app_manifest.c` `apps/app_manifest.h` `tests/sys/` `docs/sys/`                                 | `api/af_api/` `apps/` `common/` **`framework/include/`** `tests/` `docs/` `skills/`                                                                              |
| 应用层 · 业务 App | 2   | `aiapp`     | B   | `apps/demo/` `apps/app_manifest.c` `apps/app_manifest.h` `tests/app/` `docs/app/`                                   | `api/af_api/` `apps/` `common/` `tests/` `docs/` `skills/` —— **没有 `framework/`，一处都没有**                                                                          |
| 框架层          | 3   | `framework` | B   | `framework/` `tests/framework/` `docs/framework/`                                                                   | `api/af_api/` `api/fp_api/` `common/` `framework/` `platform/` `tests/` `docs/` `skills/` `apps/app_manifest.c` `apps/app_manifest.h` —— **`apps/` 只看得到清单这两个文件** |
| 平台层          | 4   | `platform`  | C   | `platform/host-fake/` `platform/metalio_eink_4_plus/` `platform/otherplatform/` `tests/platform/` `docs/platform/`  | `api/fp_api/` `common/` `platform/` `tests/` `docs/` `skills/`                                                                                                   |
| 接口层          | 5   | `api`       | D   | `api/` `apps/`（其中 `app_manifest.c` / `.h` 归 tag 1 与 tag 2）`common/` `platform/` `tests/` `tools/` `docs/` | `api/` `apps/` `common/` `framework/` `platform/` `tests/` `docs/` `skills/` —— **读不到 `apps/demo/`、`apps/system/`；`skills/` 只读，不能改**                                              |

**四条容易踩的**：

- **`api/` 的根只给 tag 5。** tag 3 拿到的是 `api/af_api/` 与 `api/fp_api/` 两条，不是整棵 `api/`——落在 `api/` 根下的文件，它读不到。想让某一层看见的东西，必须放进 `af_api/` 或 `fp_api/`。
- **`apps/` 的根归 tag 5，两个 App 目录各自归 1 和 2。** 于是接口层能放 App 层共享头，却读不到你 App 的实现。
- **`platform/` 的根归 tag 5，三个板目录归 tag 4。** 平台公共部分（`port/`、将来的 `README.md`）由接口层维护；板级实现只有 tag 4 写。
- **`apps/app_manifest.c` / `.h` 是唯一被多个 tag 共同拥有的文件。** 写：tag 1 与 tag 2（清单只有一份，系统 App 与业务 App 的条目都在里面）；读：tag 1、2、3（框架要合并清单、查重 id）。**一个文件可以有多个角色**——命中同样长度的最长前缀时全部生效（并集放行），不按行序取第一个，所以判定与遍历顺序无关。

**两处最容易被误读的读权**：

- **tag 1 拿到的是 `framework/include/`，不是整棵 `framework/`。** 管理器要 include `fw_app_state_of()` 这类控制面，否则整机起不来；但它只需要**声明**，不需要看实现。`policy.txt` 里是两行：`read framework/include/ 1 3 5` 与 `read framework/ 3 5`。
- **tag 5 拿到整棵 `framework/`。** 那是给 `tests/baseline/` 的生命周期测试直接驱动框架用的，不是让它去改实现——`framework/` 的写权在 `role 3`。

**`r` 不是保密。** 它的意思是"允许 `#include` 该路径的头"。而**能链接一个符号 = 能 include 声明它的那个头**——实现在 `.c` 里，`.c` 的读权只影响"能不能看/改源码"，不影响链接。读不到 = 编译/链接失败，不是"看不见"。真正的"能不能调某个函数"由符号判定管（见 access-control §4.2）。

---

## 2. 四个角色、五个 tag

| 角色                 | 持哪些 tag | 干什么                                            | 从哪开始                                                                           |
| ------------------ | ------- | ---------------------------------------------- | ------------------------------------------------------------------------------ |
| **A** 应用层 · 系统 App | 1       | 管理器（id 1）、各能力服务 App（显示 / 输入 / 存储 / 网络 / 电源的宿主） | [../skills/role-framework/SKILL.md](../skills/role-framework/SKILL.md) |
| **B** 应用层 · 业务 App | 2       | 真正对外的那条路径：写业务 App（默认形态是阅读器）                    | [../skills/role-app/SKILL.md](../skills/role-app/SKILL.md) |
| **B** 框架层          | 3       | 机制：生命周期、注册表、事件、伪阻塞                             | [../skills/role-framework/SKILL.md](../skills/role-framework/SKILL.md) |
| **C** 平台层          | 4       | 把一块板接进来：实现 `pf_ops_t` 九项、跑一致性套件              | [../skills/role-platform/SKILL.md](../skills/role-platform/SKILL.md) |
| **D** 接口层          | 5       | 定义 `api/` 接口、搭骨架、写门禁与文档                        | [../skills/role-api/SKILL.md](../skills/role-api/SKILL.md) + 本文 + [access-control.md](access-control.md) |

**两个对外用途，不是一主一辅**：写 App（tag 2）、把一块板接进来（tag 4）。只有 App 侧顺、平台侧得先读懂框架源码，这套东西就退化成"只有原作者能移植"。

---

## 3. 谁都不能写

| 路径 | 理由 |
|---|---|
| `policy.txt` | 它就是考卷。AI 能改考卷，所有约束一次作废 |
| `skills/` | 角色手册就是考卷的题目。v1.5 起从 `role 5` 摘掉——实测后果是 AI 会把三份 skill 列进自己的改动清单。**发现与实际不符 → 报告，不要自己改** |
| 构建文件（`CMakeLists.txt` / `partitions.csv` / `sdkconfig.defaults`） | 能改构建就能加 `-I`、改链接、让 CI 永远绿 |
| `.github/` | CI 本身。同上 |
| `AGENTS.md` | 入口文件，只有人能改。刻意不写进任何 `role` 行——入口不该由被约束的一方维护 |

### 3.1 新目录归谁

分两种情况，判据只有一条：**它的父路径有没有被某条 `role` 行覆盖**。

**① 父路径已被覆盖 → 继承。** 因为写权按最长前缀匹配，`platform/`、`tests/`、`docs/`、`apps/` 这些根已经落在某条 `role` 行里，所以：

- `platform/<新板>/` 在加前缀之前就能被 `role 5` 的 `platform/` 兜住——这是"整棵 `platform/` 归接口层"（公共部分得有人维护）的**副产品**，不是为板子设计的；所以加板的正确顺序仍是"人先加前缀，再建目录"（见 §5）。
- `tests/<新目录>/`、`docs/<新目录>/` 自动归接口层（tag 5）。

**② 仓库根下新建顶层目录 → 不属于任何人。** `policy.txt` 只列具名目录，没有 catch-all。新建 `foo/` 的结果是**谁也写不了**，必须先由人在 `role` 行里加一行。

- 这是刻意接受的代价："新建顶层目录"是低频动作，而放开 catch-all 等于把 `CMakeLists.txt`、`policy.txt` 也交出去——为省一次改动，把整个权限模型的边界拆掉，不值得。
- 失败方向因此是**安全的**（无人可写 = 门禁红）。但**平台目录不是**：漏加新板前缀不会红，而是静默归给接口层，只能靠 review 拦。

---

## 4. 停止条件（命中任一条，立刻停下来问人）

全局清单（对所有人）：

- 改分层、依赖方向、[architecture.md](architecture.md) 里的四条不变量中的任何一条
- 改 `policy.txt`（包括新增一个 tag）
- 改 App 的回调集合（`init` / `exit` / `task`）、伪阻塞语义、错误码归属
- 改**已定稿**的 `api/` 公开签名（新增一个接口不算——那是 D 的日常；改动已定稿的签名等于改契约）
- 需要某个能力但 `api/` 里没有——**报告，不要绕过**
- 烧录真机、改 CI、动 `.github/`

各角色额外一条（同一个角色持多个 tag 时，按 tag 分行；下面共五行）：

| 角色 | 额外停止条件 |
|---|---|
| A（tag 1） | 要往 `apps/system/` 里加新服务能力；要改服务注册表契约 |
| B（tag 2） | 要读 `framework/`（拿不到就是设计如此，不是权限 bug——报告需求，不要 `extern` 绕过） |
| B（tag 3） | 要把 SDK 标识符引进 `common/ api/ framework/`；要在 framework 里建任务/定时器/队列实例 |
| C（tag 4） | 要改九项硬必需的签名；要把某个能力标成"平台不提供"而这会让现有 App 编不过 |
| D（tag 5） | 要在接口里新增一个"运行期操作设备"的入口（v1 明确不做） |

停下时的交付不是"我遇到问题了"，而是：**我在做什么 / 撞到哪条边界 / 为什么不能自己解决 / 选项与代价 / 我建议哪个 / 在你回复前我不动哪些文件**。

---

## 5. 三个高频改动，各自动哪个文件

### 加一个 App（tag 2）

1. 在 `apps/demo/` 下建目录、实现你的 App。**`.h` 和 `.c` 都可以放，怎么分由你自己决定**——唯一硬约束是不要把实现放到 `apps/` 根（那里是 tag 5 的共享接口区，不是任何 App 的目录）
2. 在 `apps/app_manifest.c` 的数组**末尾追加一行**。清单**只有一份**，系统 App 的条目也在里面；这个文件同时划给了 tag 1 与 tag 2——`apps/` 根下的其它文件归接口层，只有 manifest 这两个文件是例外。你只追加自己 App 的那一行，不碰别人的条目
3. 用 `api/af_api/api.h` + `common/`，不碰 `framework/`

`apps/app_manifest.c` 是所有 App 共用的唯一一份清单——**只许追加，不许改既有行**。这条门禁还没实现：现在你能改既有行（例如把自己的 App 写成 `APP_FLAG_SYSTEM`，它就会被 `fw_boot()` 直接拉起，绕过管理器的启动策略；读权不改——权限按路径判，不看标志）。在它实现前，这条是约定；按约定违反同样按缺陷处理。

### 加一块新板（tag 4）

1. **人**做的：在 `policy.txt` 的 `role 4` 行尾追加 `platform/<新板>/`。**先加前缀，再建目录**——顺序反了的话，新目录会先落在接口层名下（见 §3.1），等再加前缀时它就有两个 tag 能写，所有权从"暂时错"变成"永久模糊"，只能靠 review 收敛
2. **tag 4** 做的：自己建目录、自己写实现，填 `pf_ops_t` 九项 + `.version`，让 `tests/port/` 一致性套件在 host 上跑绿

**没有模板可抄。** 不走 `platform/template/`（该目录已取消）。工程里会有墨水屏的例程，实现时**参考它、按契约自主实现**——不是把一个现成工程拷进来改引脚宏，那是"看起来能跑"而不是"接了进来"。

**不需要新增 tag**：平台层是"一块板一个目录"，不是"一块板一个 tag"。代价是板与板之间没有权限隔离——这个交换是刻意的：加板的成本必须低。

### 加一个 tag（人）

只有一种情况需要：新增一个**架构层**。加 tag 要同时改 `policy.txt`、本文、[access-control.md](access-control.md) §1，以及 `tools/` 里所有按 tag 索引的东西。
**不应为了"加一块板"或"加一个人"而加 tag。**
