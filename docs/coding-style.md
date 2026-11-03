# 代码编写规范（v2.0）

> **状态：v2.0（2026-11-03）。** 由旧仓库 `coding-style.md` v1.3（301 行，`git show 3e9eb7c:docs/coding-style.md`）改写：
> 删掉已不存在的路径与函数，补上两条被真机与 TSan 逼出来的并发/平台规则（§10），
> 并把"代码与本文不符"的地方集中列在 §13。**§13 的 1–6 已于同日修正，只剩第 7 条待做。**
>
> 适用范围：本仓库的 **C11 源码**（`common/` `api/` `framework/` `platform/` `apps/` `tests/`）。
> 第三方代码与生成的代码不适用。

---

## 0. 先说清楚：这里只写"机器查不了的"

本仓库有一半规矩由门禁兜底，写进文档只会变成第二个事实源。分工：

| 规矩 | 谁来保证 |
|---|---|
| 不许 `malloc` 家族、不许 `esp_*`/`driver/`/`freertos/` 头进 `common/ api/ framework/ apps/` | `tools/check_purity.py`（编译期红） |
| 公开头的依赖闭包 ⊆ 读者的 `link` 集合 | `tools/check_link.py`（`clang -M`） |
| 头文件自包含（单独 `-fsyntax-only` 能过） | `tools/check_headers.py` |
| 能力位 ↔ 接口头 双向一致 | `tools/check_caps.py` |
| 服务号 ↔ `svc-ids.md` 双向一致 | `tools/check_svc_ids.py` |
| `-Wall -Wextra -Werror`、无隐式声明 | 编译器 |
| **命名、注释四段、内存申请的五行清单、inline 规模、文件/函数拆分** | **只有评审**（本文） |

最后一行是本文存在的理由：**它们没有机器兜底，所以必须成文，否则每次评审都在重新发明标准。**

---

## 1. 一条总则

**每个符号都要能回答两个问题：它由哪一层实现、它是函数还是数据。**

- 由哪一层实现 → 决定**层前缀**（§2）。
- 是函数还是数据 → 决定**形态**（函数 §4、类型 §5、宏与枚举值全大写）。

---

## 2. 层前缀：谁实现写谁的

前缀**表示实现它的那一层，不表示谁在调用**。

| 层 | tag | 前缀 | 例 |
|---|---|---|---|
| 应用层（业务 App） | 2 | `app_` | `app_demo_desc`、`app_demo_ticks` |
| 框架层 | 3 | `fw_` | `fw_app_start`、`fw_delay_block` |
| 平台层 | 4 | `pf_` | `pf_platform_ops`、`pf_host_fake_ops` |
| 接口层 | 5 | `api_` | 版本与编译期开关；能力接口见 §2.1 |
| 公共基础 | — | `cm_` | `cm_atomic_load`、`CM_ERR_TIMEOUT` |
| 测试 | — | 文件名 `test_*` | 用例函数 `tt_case_<名>` |

- 前缀之后接**这个符号自己的名字**，不重复层名：`fw_app_start`，不写 `fw_framework_app_start`。
- **`static` 函数同样带层前缀**——它只在文件内可见，但依然属于某一层（`fw_boot.c` 里的
  `slot_at()` 是例外中的例外：纯文件内工具，见 §4 末条）。
- 宏与枚举值全大写，同样带层前缀：`PF_CAP_UART`、`CM_ERR_TIMEOUT`、`FW_APP_STATE_RUNNING`。
- **后果要认**：App 最常调的那批函数（延时、分配、日志）实现在框架层，所以带 `fw_` 前缀，
  App 里写 `fw_delay_block(100)`。前缀告诉你"出问题该找谁改"，而不是"你属于谁"。

### 2.1 例外：能力接口用能力名做前缀

`api/af_api/<能力>.h` 里给 App 用的那批函数**不按层前缀**，用人读的能力名：

| 能力 | 前缀 | 现状 |
|---|---|---|
| 资源/配额 | `res_` | `res_acquire`、`res_release` |
| 能力查询 | `cap_` | `cap_available` |
| 串口 | `uart_` | `uart_open`、`uart_write`、`uart_read` |
| 显示 / 输入 / 音频 / 存储 / 网络 / 电源 | — | **还没有头，别照着设计稿写** |

理由：能力接口的**实现方不唯一**——框架实现 `res_acquire`，平台实现串口，
将来 `stor_read` 可能是存储服务 App；按"谁实现写谁的"会把同一个头切成 `app_` 与 `fw_` 两种前缀，
而调用方根本不关心是谁实现。能力名唯一，且直接回答"这是哪个能力的接口"。

约束：

- 能力前缀只出现在 `api/af_api/<能力>.h`；**框架内部实现**（`framework/src/fw_uart.c`）
  导出的是同一批 `uart_*` 符号，不再叠加 `fw_`。
- 能力名**不得与层名重合**：`app_` `fw_` `pf_` `cm_` `api_` 保留给层前缀。
- 新增一个能力前缀 = **新增一处公共契约**，必须同时：登记 `docs/generated/catalog.md`、
  按 `skills/role-api/SKILL.md` §4 的四件套落地、跑 `./build.sh run`。

---

## 3. 变量

**snake_case：全小写，多词下划线，不用驼峰，不用匈牙利式。**

```c
int      page_index;
size_t   pixel_count;
bool     is_ready;        /* 布尔用 is_ / has_ / can_ 开头，名字本身读出真假 */
app_ctx_t *ctx;           /* 指针不加 p 前缀 */
```

- 全局变量若必须存在，同样 snake_case，并在声明处写清用途 + 谁写 + 并发约定（§11.4）。
- 计数器/标志跨任务共享时，**类型不说明并发**——注释必须说明（§10）。

---

## 4. 函数

**`<归属>_<名>[_<后缀>]`**，全小写。

### 后缀：非必要不加

只在"从名字看不出来、但调用方必须知道"时才加。目前只有一个：

| 后缀 | 何时加 | 例 |
|---|---|---|
| `_block` | 这个调用**会阻塞**（含伪阻塞：可能永远不返回） | `fw_delay_block()` |

- 后缀放最后，不能插中间（`fw_block_delay` 错）。
- 返回值、错误码不进名字（`fw_get_errno_ok` 错，用返回值）。
- 旧规范里还有一个 `_accurate`（精准延时）——现在只有 `sleep_ms`，没有这个区分，**已取消**。

### 文件内工具函数

纯文件内、不构成任何层语义的小工具（如 `fw_boot.c` 里的 `slot_at()`），可以不带层前缀，
但必须 `static`，且文件顶部有一段注释说明它属于谁。

---

## 5. 类型

**结构体、整数句柄以 `_t` 结尾；枚举以 `_e` 结尾。** 全部 snake_case。

```c
typedef struct { ... } app_desc_t;        /* 结构体 */
typedef enum { ... } fw_app_state_e;      /* 枚举 */
typedef uint16_t      fw_svc_id_t;        /* 标量别名 */
typedef struct fw_app_ctx *fw_app_h;      /* 不透明句柄：若给 typedef，用 _h 而不是 _p */
```

- 枚举**值**全大写、带层前缀：`FW_APP_STATE_RUNNING`、`PF_CAP_UART`。
- 不在类型名里重复 `struct`/`enum`：写 `app_desc_t`，不写 `struct app_desc_t`。
- **指针类型后缀：本仓库目前不用 `_p`。** 旧规范有这条，但现在的句柄一律写成
  `struct fw_app_ctx *`（不透明指针）而不是 `fw_app_ctx_p`——两个名字做同一件事只会多一处漂移。
  将来若真引入指针 typedef，用 `_h`（handle）而不是 `_p`，并在本文补一节。
- 类型前缀不强制：`app_desc_t` 里的 `app_` 指**被描述的对象**，不是写它的那一层。

---

## 6. 结构体成员

**成员不加任何前缀。**

```c
typedef struct {
    uint32_t      id;
    const char   *name;
    uint32_t      flags;
    size_t        stack_bytes;
} app_desc_t;
```

`_x` 前缀是 C++ 类成员的约定；本仓库是纯 C11，不适用。C 里读成员永远带 `.` 或 `->`，
再加前缀只是噪声。

> 将来产品仓库若引入 C++ 类：类成员以 `_` 开头，且禁止 `__x` 与 `_X`（C11 §7.1.3 保留给实现）。

---

## 7. 文件

### 7.1 命名

- 文件名、目录名全小写 snake_case：`fw_uart.c`、`pf_ops.h`、`test_uart.c`。
- **一个模块一对 `.h` / `.c`，名字相同**；`.h` 只放声明、类型、宏，不放定义（`static inline` 例外，见 §9.1）。
- 主要文件与实现的对应关系要一眼看出来：`api/af_api/uart.h` ↔ `framework/src/fw_uart.c`
  （名称相同；层前缀不同是 §2 与 §2.1 的必然结果）。

### 7.2 头文件守卫

**守卫必须满足两条：以 `ESP_AI_SKILL_` 开头、全仓唯一。** 取法：把路径大写、非字母数字换成 `_`，
去掉 `include/`、`src/` 与最外层的归类目录（`apps/` `common/` `framework/` `platform/`），末尾加 `_H`。

```c
api/af_api/uart.h            → ESP_AI_SKILL_AF_API_UART_H
framework/include/fw_app.h   → ESP_AI_SKILL_FW_APP_H
common/include/cm_types.h    → ESP_AI_SKILL_CM_TYPES_H
platform/host-fake/pf_host_fake.h → ESP_AI_SKILL_PF_HOST_FAKE_H   /* 简写，同一规则 */
```

- 不用 `#pragma once`（非 C 标准，且与其余头不一致）。文件头一段注释放在守卫之内。
- **两个板子的同名文件守卫必须不同**（`AI_PASSPORT_C3_PF_ESPIDF_H` / `ESP32S3_DEVKIT_PF_ESPIDF_H`）。
  否则谁先 include 谁生效——一旦有人在 host 测试里同时 include 两块板，就会静默拿到第一块的声明，
  而这两块的 `esp_port_caps()` 是不同的。

### 7.3 include 顺序

```
标准库
（空行）
本仓库其它头：common/ → api/ → framework/ → platform/
```

- 头文件里**标准库在前**（读者先看到依赖），本仓头按依赖方向排列。
- `.c` 里**自己的头放第一行**，其余同上。
- 只 include 用得到的头；`framework/` 与 `platform/` 的可见性由 `policy.txt` 的 `link` 行约束，
  不是靠自觉。
- 头文件必须**自包含**：单独 `cc -std=c11 -fsyntax-only -I. <头>` 能过（门禁会查）。

---

## 8. 排版

- 缩进 **4 个空格**，不用 tab。
- **控制语句**的大括号与语句同行；**函数定义**的大括号另起一行（clang-format 的 `Linux` 风格）：

```c
static bool port_open(uint32_t port)
{
    if (port >= ops()->count) {
        return false;
    }
    return true;
}
```

- 单行不超过 **100 列**。
- `if` / `for` / `while` / `else` **必须带大括号**，哪怕只有一行——
  伪阻塞的"不返回"最怕少一对括号，少一行括号就是把清理写进了死代码。
- 指针 `*` 贴变量名：`const char *name`。
- 一元运算符贴操作数；二元运算符两侧空格；`,` 后空格。
- 不留行尾空白；文件以单个换行结尾。
- **排版不靠记**：仓库根有 [`.clang-format`](../.clang-format)，它编码的就是本节；
  `./build.sh run` 里的**格式门禁**（`tools/check_format.py`）会把"与样式文件不一致"
  变成红灯，所以本节不需要背——写错了门禁会说哪个文件。

```bash
xcrun clang-format -i <file>...            # macOS 自带（Xcode 工具链）
git ls-files '*.c' '*.h' | xargs xcrun clang-format -i    # 全仓
```

  配置里有三项是刻意非默认的：`BreakBeforeBraces: Linux`（不是 LLVM 的 `Attach`）、
  `ReflowComments: false`（不重排中文注释）、`SortIncludes: false`（include 顺序有含义）。
  **不要**为了"更整齐"把它们改回默认值。

---

## 9. 内存与内联

这一节"软"：**没有门禁判定，靠评审**。所以每条都写理由——否则事后分不清是有意选的，还是没想过。

### 9.1 内联：默认不写

**默认不写 `inline`。** 只有函数体**不超过 5 行**才允许，并在旁边写一句为什么：

```c
/* 只在头里包一层取值，函数体 2 行 —— 允许 */
static inline uint32_t fw_cap_bit(uint32_t bit) { return bit; }
```

- 头文件里的 `static inline` **只允许这个规模**；大函数标 `inline` 只是让每个编译单元多一份副本。
- **不用 `#define` 宏代替函数**：宏没有类型检查、不能下断点，伪阻塞的"不返回"在宏里更容易写错
  （`CM_ATOMIC_*` 是刻意的例外：它的宽度必须由调用方的指针类型决定，见 `common/include/cm_atomic.h` 的注释）。

### 9.2 多套实现时，选静态内存占用小的那套

除非小的那套有明确代价：速度差一个数量级、可读性崩到没人敢改、需要关中断或持锁做长事。
选了大的那套，**理由写进交付清单**。

### 9.3 内存来源：栈 > 静态 > 动态

1. **能用栈就用栈。**
2. **其次静态**——但静态常驻、初始化要自己保证、多任务共享的并发由你负责。
   "省了一次分配"不能成为静态的理由。
3. **大块内存优先动态**：大缓冲放栈上会爆栈（App 任务栈见 `APP_STACK_MIN = 3072`），
   放静态则常驻不还——**该动态的时候就动态**。

**凡是用静态或动态内存，交付清单里必须单列五行**：

| 项 | 写什么 |
|---|---|
| 申请什么 | 对象/缓冲的名字与类型 |
| 多大 | 字节数（静态写常量，动态写最坏情况） |
| 谁释放 | 哪段代码负责（静态的是"谁在什么时候重置"） |
| 什么时候释放 | 正常路径之外还要写：取消、`FAILED`、重启时怎么回收 |
| 申请失败怎么办 | 返回什么错误码、上一状态怎么退回 |

**没有这一行的清单，评审不通过。** 伪阻塞下"清理只能放 `exit()`"，漏写这五行，泄漏就没人认领。

### 9.4 内存只能从 `fw_` 拿

- 唯一入口是 `fw_alloc(bytes)` / `fw_free(ptr)`（声明在 `api/af_api/app.h`）。
- **禁止** `malloc` / `calloc` / `realloc` / `free` / `strdup` / `aligned_alloc` / `posix_memalign`；
  **禁止** STL 默认分配器（`std::vector`、`std::string`、`make_unique` 等）。
- 额度：`fw_res_set_quota(ctx, bytes)`，超了是 `CM_ERR_QUOTA`（查你漏没漏释放），
  不是 `CM_ERR_NOMEM`（整机没了）——两个码分开就是为了现场能猜到该查什么。
- **例外只有一处**：`platform/<板>/` 里**实现 `pf_ops_t` 的 `mem_alloc`/`mem_free` 的那段代码**
  （`host-fake` 内部用 `malloc` 也是同一类）。**"我在平台层"不构成例外**，
  平台层的其它代码同样只能走 `fw_`。
- 旧规范说"申请要进 `res_track` 登记"——`res_track` 已删除（D25），现在槽位由
  `res_acquire(&ctx->slots[i], ...)` 直接写，资源面只有 `app/res.h` 那一套。

### 9.5 这些要人拍板

"要不要用静态内存""要不要动态申请""这个 `inline` 算不算小"——**由人决定**。
两条路径：写进清单等人批；或标"待定"，先做不依赖它的部分。

---

## 10. 并发与平台代码的两条硬规则

这两条旧规范里没有，是 2026-11 被真机与 TSan 逼出来的。

### 10.1 跨任务状态只能用 `CM_ATOMIC_*`，不许 `volatile`

- `volatile` 只挡编译器优化，**不给原子性、不给 happens-before**；C11 里两个任务读写同一个
  `volatile` 变量仍是数据竞争（UB）。TSan 在框架里一次抓出七处（D20）。
- 统一用 `common/include/cm_atomic.h`：`CM_ATOMIC_LOAD / STORE / EXCHANGE / ADD_FETCH / SUB_FETCH`
  + `CM_MEM_*` 序。状态位用 `RELEASE`/`ACQUIRE`；纯观测计数用 `RELAXED`，
  但要在注释里写明它**不提供同步语义**。
- **状态位变成终态 ≠ 任务已退出**：读到 `STOPPED` 只能说明"它决定停了"，
  任务可能还在跑最后一片阻塞。要等任务真的结束，得有另一个信号（当前实现里靠
  `fw_live_remove()` / `task_exit_current()` 的时序约定，见 `fw_app_ctx_free()` 的注释）。

### 10.2 平台层同一份代码要在两块板上都是对的

- **平台结构体必须整体清零再逐字段赋值**（`uart_config_t cfg = { 0 }`）。
  S3 上 `rx_flow_ctrl_thresh` 是栈垃圾值 → `E uart: uart_param_config(995): rx flow thresh error`；
  而 C3 碰巧是 0，一路绿灯（D59）。**"另一块板过了"不是证据。**
- `sleep_ms(n)` 的 tick 数**向上取整到至少 1**（100 Hz 下 1–9 ms 会截成 0 → 忙等，伪阻塞失效）；
  `sleep_ms(0)` 必须真让出（FreeRTOS 用 `taskYIELD()`，`vTaskDelay(0)` 不让出）。
- `task_exit_current()` **不返回**，调用方不得在其后放清理。

---

## 11. 注释

### 11.1 用中文

注释写简体中文；标识符、类型名、宏名、错误码保持英文原样，不翻译。句子写完整，
**禁止半截话**——"这里要锁"读的人还是不知道该锁什么。

### 11.2 哪些必须有

**必须**：公共头里声明的**每个函数**（§11.3）；全局变量与文件级 `static` 变量（§11.4）；
结构体里不自明的字段（单位、取值范围、谁能改）；任何"看着像普通写法、其实有约束"的地方
（必须持锁、必须最后调用、必须在中断外、失败后要重试）。

**不必**：局部变量（名字够清楚时）；`i++` 这种自明的赋值。

### 11.3 函数注释块：四段固定内容

```c
/*
 * uart_write —— 往串口写一段数据，按切片推进，每片前检查取消位。
 *
 * 入参：port      逻辑端口号，见 uart_port_count()
 *       data     缓冲首地址；NULL 且 len>0 视为参数错误
 *       len      字节数；0 立即返回 CM_OK
 *       out_written  实际写入字节数，**失败时也写**（已写出去的部分不算失败）
 * 返回：成功 CM_OK；没这项能力 CM_ERR_NOT_SUPPORTED；参数错 CM_ERR_INVALID；
 *       没打开 CM_ERR_STATE；平台连续 500ms 无进展 CM_ERR_TIMEOUT
 * 线程：任务上下文可调用，**不可在中断里调用**；内部不持锁，同一端口并发写行为未定义
 * 注意：可能被取消——取消时 `fw_run_exit()` 不返回，`uart_write()` 返回后的代码
 *       不保证执行；调用方不需要自己关端口，`exit()` 里关
 */
cm_err_t uart_write(uint32_t port, const void *data, size_t len, size_t *out_written);
```

四段缺一不可，某项没有就写"无"——**空着和不适用是两回事**，读的人分不清。

| 段 | 写什么 |
|---|---|
| **入参** | 逐个名字 + 含义 + 边界（0、NULL、超长、负数） |
| **返回** | 成功是什么；失败有哪几种；**失败时输出参数是什么状态**（未改？已清零？已写部分？） |
| **线程** | 任务上下文能否并发；要不要调用方加锁；**中断里能不能调**（不能就直接写"不可在中断中调用"） |
| **注意** | 谁负责释放/关闭；会不会阻塞（含伪阻塞：可能永远不返回）；调用顺序；时长上限；平台差异 |

**中断可用性是必答项。** 写"不可在中断中调用"比什么都不写有用——不写，别人会试。

> 术语统一叫**"四段"**。旧规范写的是"四项"，指同一件事；两处并存会让人以为还有别的格式。

### 11.4 变量注释

```c
static uint32_t g_slot_lock;  /* 护 s_live[]：0 空闲 / 1 占用，只用 CM_ATOMIC_EXCHANGE 取 */
```

全局/静态变量三样齐全：**用途 + 谁写它 + 并发约定**。宏与常量写"为什么是这个值"
（`/* 253 = APP_ID_MAX - APP_ID_MIN + 1 */`）。

### 11.5 三条禁令

1. **不写"是什么能看出来的"**：`i++; /* i 加一 */` 只是噪声。
2. **不留过期注释**：改代码必须同步改注释。**旧注释比没注释更坏**——它让下一个人按错误前提改代码。
3. **不用注释代替命名**：一个名字需要三段注释才能解释，说明名字起错了，去改名字。

### 11.6 门禁不查注释

`tools/` 里没有脚本判定注释质量，**唯一保证是评审**：评审先看"有没有四段注释块"，
没有就退回，不往下看实现。

---

## 12. 提交前自查（与 `AGENTS.md` §6 配套）

- [ ] 每个新增的公开函数都有四段注释（含中断可用性）
- [ ] 新符号的命名符合 §2 / §2.1 / §4 / §5
- [ ] 用了静态/动态内存 → 交付清单里有五行
- [ ] 跨任务共享的状态用的是 `CM_ATOMIC_*`，注释写明序与语义
- [ ] 头文件有守卫、自包含、include 顺序符合 §7.3
- [ ] 改动涉及 `platform/` → 两块板都编过、都跑过（"另一块板过了"不算）
- [ ] `./build.sh run` 绿，且新增用例有**红证**（先让它红，再让它绿）

---

## 13. 与本文不符的地方（已清）

写这份文档时逐条核对过，以下偏差已在同日的同步改动里修掉：

| # | 位置 | 原状 | 处理 |
|---|---|---|---|
| 1 | `api/af_api/mgr.h` | 用 `#pragma once` | 已改成标准守卫 `ESP_AI_SKILL_AF_API_MGR_H` |
| 2 | `common/include/cm_atomic.h` | 守卫 `CM_ATOMIC_H` | 已改 `ESP_AI_SKILL_CM_ATOMIC_H` |
| 3 | `common/include/cm_types.h` | 守卫形态与 §7.2 不符 | 按新规则它就是对的，**改的是文档不是代码** |
| 4 | 两块板的 `pf_espidf.h` | **同名守卫** `PF_ESPIDF_H` | 已区分：`AI_PASSPORT_C3_PF_ESPIDF_H` / `ESP32S3_DEVKIT_PF_ESPIDF_H` |
| 5 | `platform/host-fake/pf_host_fake.c` | 标准库头在本仓头之后 | 已按 §7.3 调整（自己的头 → 标准库 → 本仓头） |
| 6 | `docs/pf-ops-decision.md`、`实施步骤.md` | 写"四项注释" | 已统一为"四段" |

### 13.1 格式化工具（已引入）

| # | 内容 | 结论 |
|---|---|---|
| 7 | [`.clang-format`](../.clang-format)（仓库根）+ 全仓格式化 | **已做**，2026-11-03 单独一次提交：`git ls-files '*.c' '*.h'` 共 45 个文件被重排（553 增 / 436 删），`./build.sh run` 仍是 179 条 + 六道门禁 PASS，且排版自此由 `check_format.py` 持续盯着 |

顺带记两条当时的判断（写给未来的自己）：

- 配置按**现有写法**反推，不是按 clang-format 的默认风格套：函数花括号另起一行、
  控制语句同行、不重排中文注释、不重排 include。用默认 LLVM 风格会让 66 个文件全变，
  连函数签名都挪位置，那是**制造** diff 而不是收敛它。
- 本机没有 PATH 上的 `clang-format`，但 `xcrun clang-format` 一直可用（Xcode 工具链）。
  上一版 §13 写的"本机没有 clang-format，要先装"是错的——只查了 `which`，没查 `xcrun`。
