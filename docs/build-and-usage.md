# 构建框架与使用说明（详细版）

这份文档回答三件事：**这个仓库怎么搭起来的、命令怎么用、写代码时哪些边界是硬的**。
仓库根 [`AGENTS.md`](../AGENTS.md) 是十五分钟能读完的入口；这一份是它的展开——遇到
"为什么这么设计"、"这条命令到底做了什么"、"红了怎么看"的时候来这里。

事实来源优先级（冲突时按这个顺序采信）：

1. `policy.txt` —— 谁能写什么
2. 头文件（`api/af_api/`、`platform/port/`）—— 接口的权威定义
3. 本目录的 `docs/` —— 为什么这样设计
4. `实施步骤.md` —— 决策记录（§7）与施工记录（§8）
5. `AGENTS.md`

---

## 1. 这个仓库是什么

一个跑在 ESP32 全系上的 **App 开发框架**，核心命题是：让**不可信、没有嵌入式经验的 AI**
持续写出人敢合并的 App 代码，同时让换一块板不需要读懂框架源码。

三层，依赖方向单向：

```
apps/       业务 App、清单            （只 include api/af_api/ 与 common/）
   ↓
framework/  机制：生命周期、伪阻塞、注册表、事件、配额
   ↓
platform/   平台适配：九项基础设施 + 能力位
```

横切面只有两个：`api/`（唯一横向通道，App 与框架的契约）与 `common/`（类型与错误码，
放 `api/` 任何一支都会造成反向依赖）。

四条设计机制，全部是可判定的：

| 机制 | 一句话 | 边界在哪 |
|---|---|---|
| 伪阻塞 | App 只写 `for (;;) { 干活; fw_delay_block(N); }`，框架在每个阻塞点内部切片等待、每片查取消位 | `fw_delay_block()` 之后的代码**可能永远不执行**，清理只能写 `exit()` |
| 提交型接口 | 跨任务传数据"提交即拷贝"，返回后调用方缓冲区即可重用 | 表满返回 `CM_ERR_BUSY`，不是 `NOMEM` |
| 服务注册表 | 服务按 id 注册，调用方按 id 查 ready，不链接对方符号 | `NOT_SUPPORTED`（没这能力）与 `UNAVAILABLE`（崩了可重试）是两个码 |
| 机器判定 | 越权、误用 RTOS 原语、闭包穿透、能力位漂移 → 编译不过或门禁变红 | `SKIP` 不是通过，退出码 `2` 不是通过 |

---

## 2. 目录地图（放什么、谁能写）

```
.
├── build.sh              # 唯一构建入口（nobody：只有人能改）
├── .clang-format         # 全仓排版（nobody：改它等于改全仓的 diff）
├── policy.txt            # 写权/链接权的唯一来源（nobody）
├── AGENTS.md             # AI 入口（nobody）
├── 实施步骤.md            # 施工计划 + 决策 D1–D59 + 推进记录（nobody）
│
├── api/af_api/           # App 的门面：app.h res.h evt.h svc.h     [tag 5 写]
├── common/               # cm_types.h/.c：cm_err_t、错误码；cm_atomic.h：原子访问  [tag 5 写]
│
├── framework/include/    # fw_app.h（转发）、fw_embed.h              [tag 3/4 写]
├── framework/src/        # 全部机制实现 + fw_internal.h              [tag 3 写]
│
├── platform/port/        # pf_ops.h：九项契约（接板唯一必读）        [tag 5 写]
├── platform/host-fake/   # host 参考实现（pthread）                  [tag 4 写]
├── platform/ai-passport-c3/    # ESP32-C3 真板                     [tag 4 写]
├── platform/esp32s3-devkit/    # ESP32-S3 真板                     [tag 4 写]
│
├── apps/demo/            # 样板 App（80 行）                        [tag 2 写]
├── apps/app_manifest.c   # 注册清单，只许追加（tag 1 与 tag 2 共写）
│
├── tests/tinytest.h      # 测试框架（四种输出行）                   [tag 5 写]
├── tests/app/            # App 用例（跟 apps/ 走）
├── tests/framework/      # 框架用例
├── tests/platform/       # 平台契约用例（host）
├── tests/support/        # 夹具（虚拟时钟、smoke）
├── tests/tools/          # 静态自检脚本（check_entry_unique.sh）
├── tests/port/           # host 与真机**同一份**的端口/行为套件
├── tests/port/idf/       # 独立 ESP-IDF 自检固件
│
├── tools/                # 门禁脚本（考卷本身，nobody）
├── skills/               # 四份角色 skill（给 AI 的说明书，nobody）
└── docs/                 # 你正在读的这一份
```

`policy.txt` 的完整语义见第 5 节。一句话：**`tools/`、`build.sh`、`policy.txt`、`AGENTS.md`、`skills/`、
`实施步骤.md`、`.github/` 归 `nobody`——AI 发现它们有问题只能报告，不能自己改。**

---

## 3. 构建入口：`./build.sh`

### 3.1 两种模式

```bash
./build.sh          # 只编译：22 个用例二进制，编不过就退出 1
./build.sh run      # 编译 + 逐个执行 + 六道静态门禁
```

输出形态：

```
BUILD ok   test_fw_registry
BUILD FAIL test_fw_delay  (编译器输出见 build/test_fw_delay.log)
    <前 20 行编译错误>
...
PASS duplicate_id_across_manifests_blocks_boot
SUMMARY pass=N fail=0 skip=0 test_registry     ← 每份一个
check_entry_unique: PASS (framework/src/fw_main.c)
link 面: PASS（5 个 tag，16 个对外头，闭包逐条包含关系成立）
纯 C 边界: PASS（4 个目录，11 条禁止头，30 个禁止标识符）
能力目录: PASS（9 个能力位，与 docs/generated/catalog.md 双向一致）
```

### 3.2 它到底做了什么（读 `build.sh` 的五个要点）

1. **一个用例一个二进制**：`tests/**/test_*.c` 每个都有自己的 `int main`，各自链成
   `build/<名字>`。所以一个用例编不过只影响它自己，其余继续跑，最终退出码仍是 1。
2. **库源范围**：`common/src`、`framework/src`、`platform`、`apps` 下的 `*.c`，
   外加 `tests/support` 下**不叫 `test_*.c`** 的夹具实现。
   - 两块真板（`platform/ai-passport-c3/`、`platform/esp32s3-devkit/`）**被排除**：
     它们 include `freertos/`、`esp_timer.h`，host 上编不过；验收走 `tests/port/idf/`。
   - `tests/support/test_*.c` 也被排除：那是用例，混进库源会撞重复符号 `_main`。
3. **双重判定**：运行阶段，**退出码非 0 或输出里有以 `FAIL ` 开头的行**都算红。
   为什么双重——某个 `main` 忘了 `return tt_summary()` 时退出码永远是 0，只看退出码会漏。
4. **空集不是失败**：没有任何用例时打印 `no tests` 并返回 0。但 `no tests` **不许被说成"通过"**。
5. **六道门禁是 `run` 模式的一部分**，顺序固定：入口唯一 → link 面 → 纯 C 边界 → 能力目录 → 服务号 → 格式。
   任何一道红，整轮 rc=1。

### 3.3 编译标志（每一条都有原因）

```sh
-std=c11 -Wall -Wextra -Werror
-D_POSIX_C_SOURCE=200809L          # -std=c11 会关掉 glibc 的 POSIX 默认暴露
-D_DARWIN_C_SOURCE                 # 仅 macOS：把 pthread_setname_np 这类扩展要回来
-pthread
-I. -Icommon/include -Itests
CC=${CC:-cc}                       # 可以整体替换，例如塞 sanitizer
```

用 `CC` 跑 sanitizer 是官方支持的用法：

```bash
CC="cc -fsanitize=address -fno-omit-frame-pointer -g" ./build.sh run
CC="cc -fsanitize=thread -g" ./build.sh run
```

两条都应当是 **0 报告 + 179 条全绿**。TSan 会把整轮拉长到十几秒——里面有一个
"纯死循环"样本，那是故意留的，见 `tests/framework/test_heartbeat.c`。

### 3.4 红了怎么看

| 现象 | 先看哪 |
|---|---|
| `BUILD FAIL xxx` | `build/xxx.log` 的前 20 行（终端只打这些） |
| 某条 `FAIL` 行 | 行尾的 `| 文件:行号` 就是断言位置 |
| `FAIL ... -- 超时（60s）没有返回` | 用例里某个等待没有上限，或真的死锁 |
| 六道门禁某一红 | 它自己会把"哪条规则、哪个路径、为什么"打出来 |
| 退出码 2 | **环境或用法问题**，不是代码问题；先看是不是缺 `clang` |

---

## 4. 六道门禁判什么

| 门禁 | 命令 | 判据 | 典型红证（故意注入） |
|---|---|---|---|
| 入口唯一 | `tests/tools/check_entry_unique.sh` | 全仓只许 `framework/src/fw_main.c` 定义 `app_main` | 在 `apps/demo/` 放第二个 `app_main` → `FAIL ... 另有：apps/demo/x.c` |
| link 面 | `python3 tools/check_link.py` | 每个 tag 能 link 的每个头，其 `clang -M` 闭包 ⊆ 该 tag 的 `link` 集合 | 给 `framework/include/fw_app.h` 加 `#include "platform/port/pf_ops.h"` → `FAIL（4 处越界）` |
| 纯 C 边界 | `python3 tools/check_purity.py` | `common/ api/ framework/ apps/` 里不许出现 SDK 头与 RTOS 符号 | 在 `apps/demo/app_demo.c` 加 `#include "esp_log.h"` → `禁止的头 'esp_log.h'（命中 'esp_'）` |
| 能力目录 | `python3 tools/check_caps.py` | `pf_ops.h` 的 `PF_CAP_*` ↔ `docs/generated/catalog.md` 双向一致（位号、宏、接口头三处） | 目录里给 `PF_CAP_DISPLAY` 填一个不存在的 `api/af_api/disp.h` → `FAIL（1 处不一致）` |
| 服务号 | `python3 tools/check_svc_ids.py` | `api/af_api/*.h` 的 `*_SVC_ID` ↔ `docs/generated/svc-ids.md` 双向一致，且**同一个号不许被两个服务占用** | 让两个头都写 `#define X_SVC_ID 5u` → `FAIL（1 处不一致）：服务号 5 被两个宏占用` |
| 格式 | `python3 tools/check_format.py` | 每个 `.c`/`.h` 与仓库根的 `.clang-format` 一致（`clang-format --dry-run --Werror`）；本机找不到 clang-format 时打 SKIP，CI 装了所以 CI 上必查 | 把一行缩进改多两格 → `格式: FAIL（1/73 个文件与 .clang-format 不一致）` |

每个检查器都有 `--selftest`，且**至少三条负向用例**——不能红的检查器等于没有检查器：

```bash
python3 tools/check_workspace_policy.py --selftest   # policy_selftest
python3 tools/check_link.py --selftest               # link_selftest
python3 tools/check_purity.py --selftest             # purity_selftest
python3 tools/check_caps.py --selftest               # caps_selftest
python3 tools/check_svc_ids.py --selftest            # svc_ids_selftest
python3 tools/check_format.py --selftest             # check_format_selftest（2 条）
```

**要证明一个门禁真的有效**，标准动作是：注入一处违规 → 确认它变红 → 还原 → 确认
`diff` 与备份一致。只跑绿不算证据。

---

## 5. 权限：`policy.txt` 的三个词

```
default: allow          # 没写到的路径不约束（人 2026-11-02 定的口径）
role <tags> <路径...>   # 写权
nobody <路径...>        # 只有人能改，优先级最高
link <tags> <路径...>   # 可 include / 可链接的集合
```

三个词的含义，别混：

- **write** = 实现能不能放在这里、参与编译链接
- **link** = 能不能 include / 链接这里的头（**不是"能不能看"**；查看源码永远不受限）
- **nobody** = AI 一律不写；发现问题报告，不自己改

**判定顺序**：命中 `nobody` → 拒（优先于 role，否则门禁能自己解除自己）→ 命中 `role`
前缀则查身份 → 谁也不覆盖就按 `default`。

**写权不传递（D2）**：能 include 一个头，不等于能 include 它 include 的东西。公开头的
依赖闭包必须落在读者自己的 `link` 集合里——这就是"App 的编译单元里不存在平台表"的
保证方式（`link 3 4 5 platform/port/`，tag 1/2 根本不在里面）。

自检命令：

```bash
python3 tools/check_workspace_policy.py --check                    # 策略自身自洽（矛盾时退出 2）
python3 tools/check_workspace_policy.py --as 2 apps/demo/app_demo.c  # ALLOW/DENY + 理由
python3 tools/check_workspace_policy.py --as 5 tools/check_link.py   # DENY：tools/ 只有人能改
```

---

## 6. 写一个 App（最短可运行路径）

### 6.1 五步

1. 在 `apps/<你的 App>/` 建 `xxx.c`（+ 可选 `.h`），照 `apps/demo/` 的形状写三个函数。
2. 主任务必须是 `for (;;) { 干活; fw_delay_block(N); }`，**不要**自己查"我该不该停"。
3. 把 `&xxx_desc` 追加进 `apps/app_manifest.c` 的 `s_apps[]`（只许追加）。
4. 用例放 `tests/app/test_xxx.c`，用 `tests/tinytest.h`。
5. `./build.sh run`，按第 9 节的四行格式交付。

### 6.2 骨架

```c
#include "api/af_api/app.h"

static cm_err_t demo_init(app_ctx_t *ctx)
{
    (void)ctx;
    fw_log("demo: 起来了");
    return CM_OK;                      /* 非 CM_OK → 进 FAILED，exit 仍会被调一次 */
}

static void demo_exit(app_ctx_t *ctx)
{
    (void)ctx;                          /* 只有这里能写清理：阻塞点后面的代码可能不执行 */
    fw_log("demo: 走了");
}

static void demo_task(app_ctx_t *ctx)
{
    for (;;) {
        /* 干活：所有等待都走框架，取消位才有效 */
        fw_delay_block(100u);
        (void)ctx;
    }
}

const app_desc_t demo_desc = {
    .id          = 2u,
    .name        = "demo",
    .init        = demo_init,
    .exit        = demo_exit,
    .task        = demo_task,
    .stack_bytes = APP_STACK_MIN,       /* 至少 3072；平台按这个分配，不许自动抬栈 */
    .flags       = APP_FLAG_SYSTEM,     /* 只给必须最先起的 App */
};
```

### 6.3 六条铁律（违反其中任何一条，后果都不是"难看"）

1. **`fw_delay_block()` 之后的代码可能永远不执行**。取消位命中时框架直接走退出五步并不返回。
2. **清理只写在 `exit()`**。`exit()` 执行期间取消位失效（`in_exit`），所以清理里可以继续等外设。
3. **资源用槽位**：`res_acquire(&ctx->slots[i], RES_KIND_MEM, bytes, RES_LIFE_APP)`，
   框架在退出第 ③ 步回收 `RES_LIFE_APP`；`RES_LIFE_BOOT` 是借整机的，自己还。
4. **配额超了是 `CM_ERR_QUOTA`，不是 `CM_ERR_NOMEM`**：前者查自己漏没漏释放，后者查整机省着用。
5. **跨任务共享的状态不许用 `volatile`**，一律经 `common/include/cm_atomic.h` 的
   `CM_ATOMIC_*`（观测计数 `CM_MEM_RELAXED` 即可，但要明白它不提供同步语义）。
6. **内存只从 `fw_alloc`/`fw_free` 拿**，禁止 `malloc` 家族与 STL 默认分配器。

### 6.4 把它做成服务（别的 App 按 id 找你，不链接你）

```c
static cm_err_t stor_init(app_ctx_t *ctx)
{
    (void)ctx;
    return fw_svc_register(SVC_STORAGE);        /* 只许在 App 任务里注册 */
}

static void stor_exit(app_ctx_t *ctx)
{
    (void)ctx;
    (void)fw_svc_unregister(SVC_STORAGE);
}
```

调用方：

```c
cm_err_t rc = fw_svc_ready(SVC_STORAGE);
/* CM_OK              → 对方 RUNNING，可以调
 * CM_ERR_UNAVAILABLE → 对方崩了/在重启，可重试
 * CM_ERR_NOT_SUPPORTED → 这个产品没有该能力，重试没意义 */
```

跨任务传数据用 `fw_svc_submit()`——**提交即拷贝**，返回后你的缓冲区就能重用；
表满返回 `CM_ERR_BUSY`（"等会儿再来"），不是 `NOMEM`。

### 6.5 让一个 App 当管理器（控制面）

普通 App 只需要 `app.h`；**要拉起、观察、停掉、回收别的 App** 才 include `mgr.h`：

```c
#include "api/af_api/mgr.h"

uint32_t id = 0u;
if (fw_alloc_id(&id) != CM_OK) { /* 号发完了（253 个） */ }

struct fw_app_ctx *c = fw_app_ctx_new_with_id(&some_desc, id);
if (c == NULL) { /* 平台表没绑 / 没内存 */ }

if (fw_app_start(c) != CM_OK) { /* 平台拒绝建任务 */ }
/* 返回时状态还是 INIT——init() 跑在它自己的任务里。等 RUNNING 要轮询： */
while (fw_app_state(c) != FW_APP_STATE_RUNNING) { fw_delay_block(10); }

fw_app_request_cancel(c);                 /* 只是"请求"，不是"立刻停" */
while (fw_app_state(c) != FW_APP_STATE_STOPPED &&
       fw_app_state(c) != FW_APP_STATE_FAILED) { fw_delay_block(10); }
fw_app_ctx_free(c);                        /* 任务结束了才许释放 */
```

三条容易记错的：id **只增不减**（重启必换号，D18）；**所有 `fw_app_start()` 拉起的实例
都进活实例表**（D53），包括你自己造的孩子，所以"丢指针"不等于"再也找不到"；停掉的实例
**仍在表里**，直到你 `fw_live_remove()` 或 `fw_app_ctx_free()`（释放会自动摘表）。
`fw_boot()` 不是管理器 API——它只负责整机启动时拉起 `APP_FLAG_SYSTEM` 那几个。

---

## 7. 接一块板

### 7.1 只读一个头

`platform/port/pf_ops.h`。九项 + 两个成员，每一项都有一段注释写明语义、边界、失败形态。

| 成员 | 关键约定 |
|---|---|
| `version` | 等于 `PF_OPS_VERSION`；改布局必须 +1，旧表不兼容 |
| `caps` | 能力位；保留位 9–31 必须读 0 |
| `mem_alloc(bytes)` | `bytes == 0` → NULL；NULL 是唯一失败信号；无 realloc |
| `mem_free(p)` | `p == NULL` 无操作；重复释放/非本分配器 = UB（host 版主动 abort） |
| `clock_ms()` | 单调、约 49.7 天回绕、**九项里唯一可 ISR**、必须用差值比较 |
| `sleep_ms(ms)` | **必须真让出 CPU**；`ms == 0` 让出一次 |
| `mutex_create()` | 返回 NULL = 平台违约（框架自己要用锁） |
| `mutex_take(m, timeout)` | **相对**超时；`timeout == 0` 只试一次；0 / -1 |
| `mutex_give(m)` | NULL 无操作；非持有者 UB；**无 destroy** |
| `task_create(desc)` | 描述体 `{name, entry, arg, stack_bytes}`，**无优先级**；栈不足返回 NULL，不许自动抬栈 |
| `task_exit_current()` | **不返回**；调用方不得在其后放清理 |

另有两个表外钩子：`pf_platform_ops()`（返回本板表）与 `pf_platform_log(const char*)`
（日志不占用了表，否则加一列就要所有板重签）。

### 7.1.1 有额外硬件的板：把实现**交上去**（已有一项真实能力：串口）

`pf_ops_t` 只收"系统跑起来必需的机制"（内存、时钟、睡眠、锁、任务）。显示、存储、
网络、音频、串口这类能力**不进那张表**——判据是"去掉它，框架还能不能跑"。
它们走另一条缝：**板子在 `pf_platform_boot()` 里把 vtable 注册给框架**。

```c
/* platform/<板>/pf_espidf.c —— 唯一需要在板级文件里做的事 */
#include "platform/port/pf_cap.h"
#include "platform/port/pf_uart.h"

static const pf_uart_ops_t s_uart_ops = { PF_UART_OPS_VERSION, 1u, app_uart_open, ... };

/* 由 fw_startup() 调用：bind() 之后、fw_init() 之前，只会调一次，必须幂等 */
void pf_platform_boot(void) { (void)fw_cap_register(PF_CAP_UART, &s_uart_ops); }
```

- 没注册的位：`cap_available()` 回答 false，相关调用返回 `CM_ERR_NOT_SUPPORTED`；
- `cap_available()` 是"硬件报了 **且** 软件注册了"——只有其一都算不可调（D57）；
- **不需要弱符号、不需要空实现、不需要 `#if`**：板子没有这个能力，就一个符号都不用提供；
- 注册的指针**活到关机**（框架只存指针，不拷贝、不释放）；重复注册返回 -1；
- vtable 的 `version` 不符 = **这项能力当作不存在**（App 看到 `CM_ERR_NOT_SUPPORTED`）；
  `CM_ERR_UNAVAILABLE` 留给"能力在、但这一次调用失败"。这条约定写在 `pf_cap.h` 规则 4。

**加一项能力要动四处（D58，串口是先例）：**

| 位置 | 写什么 |
|---|---|
| `platform/port/pf_<cap>.h` | 平台的 vtable：`version` / `count` / 各函数。**返回字节数或 0/-1，不返回 `cm_err_t`**（平台层不产出错误码，D33） |
| `platform/<板>/pf_espidf.c` | 实现 vtable + 在 `pf_platform_boot()` 里注册；`caps()` 点亮对应位 |
| `api/af_api/<cap>.h` | App 门面：纯 `cm_err_t` + 四段注释；位号用 `CAP_<CAP>` |
| `framework/src/fw_<cap>.c` | 翻译与纪律：先 `cap_available()`、切片阻塞点调 `fw_cancel_point()`、记账、`fw_reset()` 里 `fw_<cap>_forget()` |

配套还要：`platform/port/pf_ops.h` 的 `PF_CAP_<CAP>` 与 `PF_CAP_RESERVED_MASK`（位从保留区划走时两者一起改）、
`api/af_api/cap.h` 的 `CAP_<CAP>`、`tests/platform/test_stack_contract.c` 的 `_Static_assert`、
`docs/generated/catalog.md` 一行（门禁双向绑定）、`tools/check_caps.py` 的 `RESERVED_FIRST_BIT`。

**串口这条链的现状**：`uart_open/close/write/read` + `uart_port_count()` 已可用；
host 用 `platform/host-fake` 的假串口（`hf_uart_feed/take/reset` 是测试钩子），
两块真板用 `UART_NUM_1`（C3 `TX=GPIO21/RX=GPIO20`，S3 `TX=GPIO17/RX=GPIO18`，引脚是板级宏）。
**未验**：对端到底收到没有——要另一头接串口助手看，这是套件的能力边界。

> 屏幕（墨水屏/液晶屏）影响体验最大、两类刷新语义差得远，仍然**留到后面单独定方案**。

### 7.2 三个最容易写错的地方（都踩过）

- `sleep_ms(0)` 用 `vTaskDelay(0)` **不让出** CPU → 用 `taskYIELD()`；
- `ms > 0` 换成 tick 必须**向上取整到至少 1**（100 Hz 下 1–9 ms 会截成 0 → 忙等）；
- `task_exit_current()` 之后那行**不会执行**（host 的 pthread 与 FreeRTOS 行为一致，都有用例钉住）。

### 7.3 验收：host 与真机同一份用例

```bash
./build.sh run                                   # host：tests/port/test_port_conformance.c 14 条
cd tests/port/idf && idf.py set-target esp32s3   # 真机：同一份源文件，另一套构建
idf.py build && idf.py -p <port> flash
perl -e 'alarm 25; exec @ARGV' idf.py -p <port> monitor
```

`tests/port/idf/components/port_c3/CMakeLists.txt` 按 `IDF_TARGET` 自动选端口
（`esp32s3` → `platform/esp32s3-devkit`，否则 `platform/ai-passport-c3`）。

板间差异只在本目录：C3 单核（`portNUM_PROCESSORS == 1`）、S3 双核 + 8 MB PSRAM。

---

## 8. 测试

### 8.1 `tests/tinytest.h` 的输出契约

只认这四种行，别的都算噪音：

```
PASS <用例名>
FAIL <用例名> -- <说明> | <文件>:<行号>
SKIP <用例名> -- <原因>
SUMMARY pass=N fail=M skip=K <程序名>
```

API：`TT_CASE(name)` 定义用例、`TT_RUN(name)` 调用、`TT_ASSERT_INT/TRUE/FALSE/NOT_NULL/STR`、
`TT_SKIP(reason)`、`tt_summary()`（返回 `fail == 0 ? 0 : 1`）。

- 用例必须写在**文件作用域**（`TT_CASE` 展开成函数定义），不能塞进 `main` 里。
- POSIX 上有 60 秒 alarm 兜底（`-DTT_TIMEOUT_SEC=N` 可改），超时会打一行 `FAIL ... 超时没有返回`。
  **设备上这条不生效**：挂死表现为看门狗复位，不是 FAIL。
- 测试里的"取干净"循环必须有上限——无界循环会把"该红的"变成"卡住"（D50 就是这么来的）。

### 8.2 目录归属

| 源码 | 用例 |
|---|---|
| `apps/demo/` | `tests/app/` |
| `framework/` | `tests/framework/` |
| `platform/<板>/` | `tests/platform/`（host 契约）+ `tests/port/`（host 与真机同一份） |
| `tests/support/` | 夹具，可带 `.c` 实现（不能叫 `test_*.c`） |

### 8.3 红证的固定流程

1. 注入一处违规；2. **先看 `./build.sh` 的退出码**（编译失败时旧二进制还在，会跑出假绿）；
3. 看运行输出确实出现预期的那条 `FAIL`；4. 还原；5. `diff` 与备份一致；6. 重跑全绿。

改过源码之后不要直接跑 `./build/<名字>`——那是上次的二进制，先 `./build.sh` 重编。

---

## 9. 交付格式（四行，缺一段算没交付）

```text
Build:        PASS / FAIL / NOT RUN
Host tests:   PASS / FAIL / NOT RUN（贴数字）
Device tests: PASS / FAIL / NOT RUN（没做就写 NOT RUN）
Unverified:   还有什么没验证、为什么
```

- **`SKIP` 不是通过，退出码 `2` 不是通过。**
- 编译通过 ≠ 硬件验证通过；设备连着 ≠ 获得烧录授权。
- 用真实内存/栈/动态分配要单列：申请什么 / 多大 / 谁释放 / 何时释放 / 失败怎么办。

---

## 10. 常见失败速查

| 现象 | 原因 | 处置 |
|---|---|---|
| `BUILD FAIL ... duplicate symbol '_main'` | `tests/support/` 里的 `test_*.c` 被当成库源 | 命名改成非 `test_` 前缀 |
| 某二进制跑出全绿但其实没编过 | 用了旧二进制 | 先 `./build.sh`，再看 rc |
| `FAIL ... 期望 X, 实际 Y`，X/Y 差 1 | 计数跨用例累加，或清了不该清的状态 | 在 `reset_all()` 里补清零 |
| 明明加了 `assert` 却进不去 | `assert` 被 `-DNDEBUG` 吃掉 | 用 `TT_ASSERT_*`，不要用 `assert` |
| `error: '/*' within block comment` | 注释里写了 `framework/**` 这类路径 | 别在注释里写 `/*` |
| 设备上 `Guru Meditation ... Stack protection fault` | `APP_STACK_MIN` 不够（1024 必死） | 现在下限是 3072 |
| `FreeRTOS Task "x" should not return, Aborting now!` | 任务函数直接 return | 结尾 `vTaskDelete(NULL)` |
| 串口只有 `task_wdt: Task watchdog got triggered` | 高优先级任务把观测者饿死 | 观测者优先级不能被压过 |
| `assert failed: xTaskCreatePinnedToCore ... xCoreID < 1` | 写死绑到 core 1，而 C3 是单核 | 按 `portNUM_PROCESSORS` 自适应 |
| 烧录报 `ninja: subcommand failed`，串口被占 | 上一个 monitor 的 python 还活着 | `for p in $(lsof -t /dev/cu.xxx); do kill -9 $p; done` |
| `idf.py build` 报 sdkconfig 与 CMakeCache 目标不一致 | 换过 target | `idf.py fullclean` 后重新 `set-target` |

---

## 11. 文档地图

| 想知道 | 读 |
|---|---|
| AI 协作者的最短入口 | [`../AGENTS.md`](../AGENTS.md) |
| 九项端口为什么只有九项、逐条裁决 | [`pf-ops-decision.md`](pf-ops-decision.md) |
| 每个错误码谁产出、调用方该做什么 | [`error-codes.md`](error-codes.md) |
| A1–A9 验收线怎么算过 | [`acceptance.md`](acceptance.md) |
| 能力位 ↔ 接口头的映射 | [`generated/catalog.md`](generated/catalog.md) |
| 产品仓库怎么把本仓库当组件引进去 | [`cross-repo.md`](cross-repo.md) |
| 决策与施工记录（D1–D59） | [`../实施步骤.md`](../实施步骤.md) |

---

## 12. 当前基线（2026-11-03）

| 项 | 数字 |
|---|---|
| host 用例 | **179 条**，24 个二进制，`./build.sh run` rc=0 |
| 门禁 | **6 道**全 PASS（入口唯一 / link 面 5 tag·**20** 头 / 纯 C 边界 4 目录 / 能力目录 **10** 位 / 服务号 0 个 / 格式 73 个文件） |
| 真机 | ESP32-C3 与 ESP32-S3 各 **41 条**（14 端口 + 13 行为 + 6 事件 + 8 资源），`DEVICE_DONE rc=0` |
| sanitizer | ASan 0 报告、TSan 0 报告 |
| 端口 | `platform/port/pf_ops.h` 九项 + `caps()` + `version` |
| App 门面 | `api/af_api/{app,res,evt,svc,cap,uart}.h` + 控制面 `mgr.h`（**只有串口一项能力**；显示/输入/音频/存储/网络/电源仍没有头） |

确认没坏的最短方式：

```bash
./build.sh run && echo "全绿"
```
