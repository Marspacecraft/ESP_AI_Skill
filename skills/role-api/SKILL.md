---
name: role-api
description: 在 ESP AI Skill 仓库里定义或修改公共契约时使用——动 api/af_api/*.h、common/（cm_types、cm_atomic）、platform/port/、加错误码、加能力位与能力目录、改 tests/tinytest.h 或 tests/support、写 docs/。用户说"加个能力""公开这个接口""错误码怎么分""定义契约""该不该动 policy 语义""写文档"时先读这份。
---

# 角色 · 接口层（tag 5）

这一层只做一件事：**定义别人必须遵守的形状**。你不实现业务，也不接硬件；
你写下的每个函数签名、每个错误码数值、每个能力位，别人都要照着活。

所以这一层的失败模式和别处相反：不是"跑不起来"，而是**跑起来了但契约是错的**——
所有人都在错误的地基上盖楼，而且要等很久才炸。

全局规矩（交清单、四行交付、停手）见 [`AGENTS.md`](../../AGENTS.md)。

---

## 1. 动手前：交清单，等批准

清单协议在 [`AGENTS.md`](../../AGENTS.md) §0。本角色加一个公开符号会牵动五处，
清单里要把它们一次列全（漏一处门禁就会红，但那时已经写完了）：

- `api/af_api/<x>.h`（契约）+ 实现；
- `docs/generated/catalog.md`（能力位）或 `docs/generated/svc-ids.md`（服务号）的登记行；
- `docs/error-codes.md`（新错误码的话）+ `common/include/cm_types.h`；
- `docs/build-and-usage.md` / `docs/acceptance.md` 的数字与条目；
- 要不要人改 `tools/`（门禁本身）——这条我改不了。

批准了才动手——**任务描述不是批准**。

## 1.1 确认你写得了

```bash
python3 tools/check_workspace_policy.py --as 5 api/af_api/app.h
# ALLOW  api/af_api/app.h   role 5：api/    （退出码 0）

python3 tools/check_workspace_policy.py --as 5 framework/src/fw_ctx.c
# DENY   framework/src/fw_ctx.c 属于 role 3；--as 5 不在其中   （退出码 1 → 停）
```

你的写区：`api/`、`common/`、`platform/port/`、`tests/support/`、`tests/tinytest.h`、`docs/`。

**碰不到**：`framework/`、`apps/`、`platform/<板>/`，以及 `tools/`（考卷归 `nobody`：
门禁脚本有 bug 属于契约变更，走评审，不走"顺手改一下"）。

---

## 2. 契约住在哪（放错会被门禁抓住）

| 头 | 读者 | 内容 |
|---|---|---|
| `api/af_api/app.h` | App、框架、平台 | `app_ctx`、`app_desc_t`、`fw_manifest_t`、`fw_delay_block`、`fw_alloc/free`、日志、心跳 |
| `api/af_api/res.h` | App | 槽位式资源、配额控制面 |
| `api/af_api/evt.h` | App | 事件环形队列（含 ISR 版） |
| `api/af_api/svc.h` | App | 服务注册表 + 提交型接口 |
| `api/af_api/cap.h` | App | 能力位查询（`cap_available`）；位号与 `pf_ops.h` 编译期钉住 |
| `api/af_api/uart.h` | App | 第一个真实能力（串口）；新能力照它抄四件套 |
| `api/af_api/mgr.h` | App（管理器） | 控制面：发号、造实例、启停、观察、回收。**使用面与控制面分文件**，因为两者失败模式不同 |
| `common/include/cm_types.h` | 所有人 | `cm_err_t` 与字符串 |
| `common/include/cm_atomic.h` | 所有人 | `CM_ATOMIC_*` 五个宏 + 内存序枚举 |
| `platform/port/pf_ops.h` | 框架、平台 | 九项原语（App 看不到，这是刻意的） |

**`framework/include/fw_app.h` 只是转发头**（只 include `api/af_api/app.h`）。D44 之前契约
长在框架里，结果"框架要写 `app_ctx`、却 include 不了 `api/`"这个说法把所有权搞反了。
**不要把契约搬回去。**

---

## 3. 这一层独有的坑

| 现象 | 原因 | 做法 |
|---|---|---|
| `check_link.py` 报"tag 1/2 的闭包里有 platform/port/..." | 公开头 include 了只给框架/平台的东西 | 读权不传递（D2）：**错在你这个头**，不在调用方。改头，别改规则 |
| 加了能力位，`check_caps.py` 红 | 目录没登记 / 登记了不存在的宏 / 写了不存在的接口头 | 位 → `docs/generated/catalog.md` 一行（bit/名字/宏/接口头/说明）；头文件必须真存在 |
| 调用方 `switch` 里永远进不去某个分支 | `CM_ERR_CANCELED` **当前无产出者**（取消位命中直接走退出、不返回） | 头注释写明"预留，不要用它做分支"（D35） |
| 错误码数值被"整理"了 | 数值即契约，只能追加 | 新码排在末尾；`error-codes.md` 同步产出点 |
| 平台层开始返回 `cm_err_t` | 九项原语只认 `NULL`/`-1`，翻译是框架的事 | 端口里别 include `cm_types.h` 的语义 |
| 头文件很干净，但 App 一 include 就编译不过 | 头不自包含（缺 include、或用到了未声明的类型） | 每个公开头都能单独 `cc -std=c11 -fsyntax-only -I. <头>` 通过 |

---

## 4. 加一个东西的固定顺序

**加一项能力**（串口是唯一做过的样本，照它抄）：

0. 先分清两件事：**能力位**（`caps()` 里"这块板有这个硬件"）与**能力实现**（平台交上来的
   vtable）。两者缺一不可——只有位没有实现，App 一调就是空指针；只有实现没有位，
   板子在报假能力。`cap_available(bit)` 判的就是"位 ∧ 已注册"（D57）。

1. `platform/port/pf_<cap>.h`：平台的 vtable，带 `version`/`count`，返回**字节数或 -1**，
   **不返回 `cm_err_t`**（平台层不产出错误码，翻译是框架的事）。规则写进头注释：
   逻辑编号（换板不改 App）、`timeout_ms` 是上界、只能任务上下文。
2. `platform/port/pf_ops.h`：加 `PF_CAP_<CAP> (1u << n)`，**同时收窄 `PF_CAP_RESERVED_MASK`**
   （串口把位 9 从保留区划走，掩码 `0xFFFFFE00` → `0xFFFFFC00`）。
3. `platform/port/pf_cap.h` 的 `PF_CAP_COUNT` +1，`framework/src/fw_cap_reg.c` 的 `bit_index()`
   加 `case`——**漏一处就是注册失败**，表现是 `uart_port_count()==0` 这种静默假象。
4. `api/af_api/<cap>.h`：App 门面，纯 `cm_err_t`；读超时用 `CM_ERR_TIMEOUT` 表示"还没有"，
   不是错误。四段注释写全。
5. `framework/src/fw_<cap>.c`：切片 + 每片前 `fw_cancel_point()` + 记账；
   `fw_reset()` 里要调 `fw_<cap>_forget()`。
6. 板子在 `pf_platform_boot()` 里 `fw_cap_register(PF_CAP_<CAP>, &ops)`——这个钩子在
   `fw_startup()` 里于 `bind()` 之后、`fw_init()` 之前调一次，**没有别的时机**。
7. `docs/generated/catalog.md` 加一行（bit / 名字 / 宏 / 接口头 / 说明）；接口头写了就**必须真存在**。
8. `tests/port/test_stack_contract.c` 加 `_Static_assert(CAP_X == PF_CAP_X)`——App 读不到
   `pf_ops.h`，`cap.h` 只是复述位号，漂了不会报错，只能编译期钉。
9. `./build.sh run`：`check_caps.py` 四条判据 + `link 面` 必须绿；再用红证证明判据真的会红。

**加一个公开函数**：

1. 先问它属于**使用面**还是**控制面**（能不能强制别人）。
   控制面也住 `api/af_api/`，但**必须单独成文件**并在头注释顶部写明它是控制面——
   `mgr.h` 就是先例（D52）；把两种面混进同一个头，App 作者会以为控制面也能随便调。
2. 声明进对应头，四段注释写全：入参（含边界）/ 返回值（含失败码与输出参数状态）/
   线程与中断可用性 / 注意事项（谁释放、会不会阻塞、调用顺序）。
3. 实现放在能被 link 到的地方（`common/src` 或 `framework/src`）——放 `framework/src` 的函数
   就自动对 App 不可见，别在那儿放该公开的东西。
4. 用例放 `tests/framework/` 或 `tests/support/`；先写红证。
5. 跑 `./build.sh run`：`link 面` 与 `能力目录` 两道门禁就是给这一层的。

**加错误码**：`cm_types.h` 枚举末尾追加 + `cm_err_str` 一行 + `docs/error-codes.md` 产出点。

---

## 5. 你还要守文档纪律

- **一个事实只有一处**：状态写在 `实施步骤.md` 的勾选项与 §8；决策写 §7 的 D 号；
  术语写在 `AGENTS.md` 的术语表或文档里的一处。别处只链接，不复制。
- 改了实现就要回头改文档里过期的数字（条数、对外头数、板数、决策号上限）。这类漂移
  已经发生过五次，每次都是别人先发现。
- `docs/README.md` 是索引：新文档要挂进去，否则等于没写。

---

## 6. 交付前自查

```bash
./build.sh run            # 六道门禁：entry_unique → link → purity → caps → svc-ids → format，全绿
```

- 四行交付里 `Unverified` 要写清"契约有没有被真实调用方验过"——只有测试在调，就没有。
- 改了 `common/` 或头文件后，ASan/TSan 也要跑一遍（`CM_ATOMIC_*` 与类型宽度就是在这里定的）。

---

## 7. 该读什么

| 想知道 | 读 |
|---|---|
| 错误码归属与各码的语义 | `docs/error-codes.md` |
| 能力位 ↔ 接口头的登记表 | `docs/generated/catalog.md` |
| 九项为什么是这九项 | `docs/pf-ops-decision.md` |
| 可执行验收线 | `docs/acceptance.md` |
| **命名 / 注释 / 排版 / 内存怎么算对** | [`docs/coding-style.md`](../../docs/coding-style.md)（§12 是提交前自查表） |
| 全局规矩、四行交付、停手条件 | [`AGENTS.md`](../../AGENTS.md) |

---

做完按 [`AGENTS.md`](../../AGENTS.md) §6 交：四行状态 + **实现了什么**（对照清单逐条）+ **需要用户处理什么**（提交 / 授权 / 改考卷 / 拍板）。
