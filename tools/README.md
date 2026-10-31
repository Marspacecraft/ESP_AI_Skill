# tools/ —— 机器判定

这里的东西只做一件事：把 `docs/` 里写下的规则变成**能跑出的红绿**。
它们不含任何业务逻辑，也不产生代码。

> **`tools/` 没有 `read` 行。** 在 `policy.txt` 里它只有 `role 5` 的写权，
> 没有任何 `read` 行 —— 也就是说**谁都读不到这里的源码**。
> 这是设计：门禁是考卷，被考的人不该能改它，也不必读它。
> 所有身份的使用方式是**看退出码**，不是看实现。

## 退出码约定

| 码 | 含义 |
|---|---|
| `0` | 通过 |
| `1` | **代码有问题**（该改的没改、越了层、写了别人的目录……） |
| `2` | **环境问题**（没有编译器、没有 git、没有 nm）|

`2` 不是通过。`SKIP` 也不是通过 —— 它是"无东西可跑"（比如 `tests/` 还是空的）或
"判不了"（比如仓库还没有任何提交）。两者都会在输出里明确写出来。

## 一次跑完

```bash
python tools/validate.py            # 本地：未声明身份时写入判定打印 SKIP
python tools/validate.py --as 2     # 以 tag 2（业务 App）的身份跑
python tools/validate.py --as 2 --strict   # CI：SKIP 也算失败
python tools/validate.py --only purity,layers
python tools/validate.py --list
```

`validate.py` **不因前一项失败而中断** —— 一次拿到完整清单，比修一个跑一次省事。
末尾有汇总表；`1` 优先于 `2`。

## 九个门禁各查什么

| 脚本 | 查什么 | 对应设计 |
|---|---|---|
| `policy.py` | `policy.txt` 数据自检：tag 集合、前缀重复、写 ⊆ 读、read 引用未知 tag；另提供 `role <路径>` 反查 | `docs/access-control.md` §2 |
| `check_structure.py` | ①每个已存在的顶层目录必须有人能写 ②每条 `policy` 前缀必须出现在 `docs/code-structure.md` 的目录树里 | `docs/code-structure.md` §1 |
| `check_purity.py` | `common/` `api/` `framework/` 里不得 include 平台专有头、不得出现 SDK/RTOS 标识符（规则在 `purity_rules.txt`） | `docs/code-structure.md` §2.2 |
| `check_layers.py` | include 矩阵：谁可以 include 谁（逐层白名单） | `docs/code-structure.md` §2 |
| `check_policy.py` | **三条判定**：读权（`clang -M` 真实展开）／符号（`llvm-nm -u` ＋ 两问）／写入（`git diff` 路径前缀）；外加清单"只许追加"与顶层目录覆盖 | `docs/access-control.md` §4 |
| `check_catalog.py` | 能力头 ↔ `fw_svc_register(SVC_*)` 双向比对（`disp` 因 v1 不含屏幕而豁免） | `docs/architecture.md` §7.1 |
| `check_blocking.py` | App 任务体里必须有伪阻塞调用；只在无限循环里转 → `BLK-LOOP` | `docs/architecture.md` §3.1.5 |
| `check_headers.py` | 每个 `.h` 自包含（单独 `#include` 能编过） | `docs/code-structure.md` §3 |
| `run_tests.py` | **转发壳**：`tests/test.py` 的旧入口名，`--src` 已不再接受。测试入口与断言头在 `tests/`，设计见 `docs/test-plan.md` | `docs/test-plan.md` §3 |

`--as` 有两种取值：tag 数字（`1`–`5`）或 `human`。`human` 不受 `role` 行约束。

## 三条判定的实现约定

- **读权**：用 `clang -M` 真实展开一个 `.c` 的 `#include`，每个仓库内头都必须落在
  该 tag 的 `read` 集合里。缺 `read` 行 = 编译/链接失败，不是"保密"。
- **符号**：`llvm-nm -u` 列出未定义符号，逐个判**两问** ——
  ① 它的**定义**在调用者**可写**区里吗？是 → 自己的代码，放行；
  ② 否则它必须在**调用者不可写**的头里被声明，且那个头被实际 include 过。
  第 ① 问不能省：不然"自己的头里声明自己的 App 描述体"会被误判。
- **写入**：`git status` / `git diff` 里的每个路径，都必须被 `--as` 声明者的
  `role` 前缀覆盖。仓库还没有任何提交时判不了，会如实打印 `SKIP` 并提示用
  `--paths` 指定要判的文件。

## 为什么不用管道抓子进程输出

`common.run()` 把 stdout/stderr 重定向到**临时文件**再读回，而不是 `capture_output=True`。
原因：某些沙箱（Windows 上创建管道）会直接拒绝，子进程连启动都失败。
同一份理由记在 `docs/code-structure.md` 决定 10。

## 验证到哪一步了（别把"跑过"当"验证过"）

- `policy.py` / `check_structure.py` / `check_policy.py` / `validate.py`：在**当前仓库**
  （只有文档与 `policy.txt`）上真跑过，包括 `--as 5`、`role <路径>`、`--paths`、
  `CI=1` 无身份判红这些分支。
- `check_purity.py` / `check_layers.py` / `check_headers.py`：用**故意违规的探针文件**
  跑过一次（`esp_err_t` + `freertos/FreeRTOS.h` 被判 PUR；`apps/demo` include
  `framework/include/` 被判 LAY-CROSS；缺 include 的头被判 HDR-SELF），探针已删除。
  也就是说**规则确实会红**，不是永远绿。
- `check_catalog.py` / `check_blocking.py`：**还没有被真实内容跑过**
  ——仓库里没有能力头、没有 App 任务体。它们的第一次真实执行
  会发生在第一批代码落地时，届时先看它们是 SKIP 还是真 PASS。
- `tests/test.py` + `tests/tinytest.h` 已在 `tests/tools/test_gates.py` 上真跑：
  `python tests/test.py tools gates` → 12 条全 PASS；`python tools/validate.py` 第 9 步
  就是它（`pass=12 fail=0 skip=0`）。C 断言头 `tinytest.h` **还没有被任何 C 用例用过**。
- 读权与符号判定（`check_policy.py --reads/--symbols`）依赖 `clang` + `llvm-nm`；
  本机没有 `cc`，脚本会自动退到 `clang`。**这两个判定一次都没有在真实 `.c` 上跑过。**

