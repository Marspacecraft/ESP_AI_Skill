---
name: role-app
description: 在 ESP AI Skill 仓库里新建或修改一个业务 App 时使用——涉及 apps/<app>/、往 apps/app_manifest.c 追加、补 tests/app/ 用例，或用户说"写个 App""这个 App 起不来/停不下来/退出没清理/内存漏了""把它做成服务"。只要你要往 apps/ 下放代码，先读这份。
---

# 角色 · 业务 App（tag 2）

你在写"被框架管着跑"的那一小段代码：三个回调 + 一个描述体。框架负责拉起、切片等待、
在取消时替你退出。**你的 App 不许自己管生命周期**——那正是这套设计要消灭的东西。

全局规矩不在本文件里：交清单、四行交付、停手条件见 [`AGENTS.md`](../../AGENTS.md)。
这里只写这一层独有的判断。

---

## 1. 动手前先确认你写得了

```bash
python3 tools/check_workspace_policy.py --as 2 apps/demo/app_demo.c
# ALLOW  apps/demo/app_demo.c   role 2：apps/demo/         （退出码 0）

python3 tools/check_workspace_policy.py --as 2 framework/src/fw_ctx.c
# DENY   framework/src/fw_ctx.c 属于 role 3；--as 2 不在其中   （退出码 1 → 停，去报告）
```

**DENY 就是停**，不是"求个例外"。你的写区：

| 能写 | 说明 |
|---|---|
| `apps/<你的 App>/` | 新建目录即可；不用登记到 `policy.txt` |
| `apps/app_manifest.c` / `.h` | **只许追加数组元素**，不许重排、不许改别人的行 |
| `tests/app/` | 你的用例放这里 |
| `docs/app/` | 你这个 App 的设计说明（可选） |

**碰不到**：`framework/`、`platform/`、`api/`、`common/`、`tools/`、`build.sh`、`policy.txt`、
`AGENTS.md`。要改它们 = 换身份，先报告。

App 能 include 的只有六个头：`api/af_api/{app,res,evt,svc,cap,uart}.h`（使用面）与 `mgr.h`（控制面，
只有管理器角色要用）。看到别的名字就当它不存在。

---

## 2. 一个 App 的骨架

复制 `apps/demo/`（82 行，含注释）的形状：

```c
static cm_err_t my_init(app_ctx_t *ctx) { (void)ctx; return CM_OK; }
static void     my_exit(app_ctx_t *ctx) { (void)ctx; /* 清理只写这里 */ }
static void     my_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        /* 干活 */
        fw_delay_block(100u);   /* 一切等待都走框架 */
    }
}

const app_desc_t my_desc = {
    MY_ID, "my-app", my_init, my_exit, my_task,
    0u /* 不是 SYSTEM */, APP_STACK_MIN,
};
```

然后往 `apps/app_manifest.c` 的 `s_apps[]` **追加一行** `&my_desc`。

### 三条形态约束（违反哪条都会变成难查的 bug）

1. **主任务不许有状态机**。不要写 `if (should_stop) break;`——取消协议已经在框架里，
   复制到每个 App 里就等于没有统一出口。
2. **清理只在 `exit()` 里**。`fw_delay_block()` 之后的代码**可能永远不执行**：取消命中时
   框架直接 `fw_run_exit()`，不返回。写在延时后面的清理 = 一段死代码。
3. **`init()` 只做一次性准备**，失败返回非 `CM_OK`，框架会走 FAILED 并调 `exit()`。

---

## 3. 这一层独有的坑（都踩过）

| 现象 | 原因 | 做法 |
|---|---|---|
| 板上 `Guru Meditation ... Stack protection fault`，只打了一行日志 | `APP_STACK_MIN` 曾经是 1024，而 `fw_log` 的 256 字节栈缓冲 + `vsnprintf` 吃不住 | 现在下限是 **3072**；描述体里写 `APP_STACK_MIN`，别自己写小数字 |
| 重启后两个实例状态互相污染 | 复用同一个 id | id 从 `2..254` 里手工挑一个**清单里没出现过的**；框架重启必换号（D18） |
| TSan 报一堆 data race | 用 `volatile uint32_t` 当跨任务计数 | 一律 `CM_ATOMIC_*`（`common/include/cm_atomic.h`），观测计数用 `CM_MEM_RELAXED`，并明白它**不提供同步语义** |
| 提交数据后画面/日志偶尔是旧的 | 以为 `fw_svc_submit()` 是零拷贝 | 它是**提交即拷贝**：返回后你的缓冲区可以立刻重用。反过来，表满返回 `CM_ERR_BUSY`（"等会儿再来"），**不是** `NOMEM` |
| 退出了但资源没还 | 把句柄存在自己的变量里 | 用 `res_acquire(&ctx->slots[i], RES_KIND_MEM, bytes, RES_LIFE_APP)` 写进上下文槽位；框架在退出第 ③ 步替你收 `RES_LIFE_APP` |
| 别的 App 找不到你 | 以为能链接符号 | App 之间**只能按 id 找人**：`fw_svc_register(id)` / `fw_svc_unregister(id)`，调用方先 `fw_svc_ready(id)` |

---

## 4. 标准流程

1. **交清单**（AGENTS §0）：要建/改哪些文件、每个属于哪个 tag、为什么不能只改现有的、大概多少行。
2. 等一句明确的"可以"。用户说"实现 X"**不算**批准。
3. 写 App → 追加清单 → 写 `tests/app/test_<app>.c`。
4. `./build.sh run`：必须 **rc=0 + 全绿**。只看某一份：`./build/test_my_app`（**先跑 `./build.sh` 重编**，
   否则你测的是旧二进制——这个坑踩过两次）。
5. 新用例**先写成会失败的版本**跑一遍（红证），再让它变绿。红证时先看 `./build.sh` 的退出码，
   编译失败时旧二进制还在，会跑出假绿。
6. 按 AGENTS §6 的四行交付。改了 App 逻辑但要上板才算数，就写 `Device tests: NOT RUN` 并说清楚。

---

## 5. 现在的边界（别自己造）

- **管理器要另 include `api/af_api/mgr.h`**（控制面）：`fw_alloc_id()` 领号 →
  `fw_app_ctx_new_with_id()` 造实例 → `fw_app_start()` 拉起 → `fw_app_state()` 观察 →
  `fw_app_request_cancel()` 停 → 终态后 `fw_app_ctx_free()` 释放。
  两个容易记错的点：**所有 `fw_app_start()` 拉起的实例都进活实例表**（D53，停掉的也留着，
  直到摘表或释放——释放会自动摘表）；`fw_app_start()` 返回时状态还是 `INIT`，要等 `RUNNING` 得自己轮询。
- **没有任何能力头**：显示、输入、音频、存储、网络、电源都还没有。别照着旧文档写
  `disp_commit` —— 它不存在。
- 纯死循环框架发现不了也停不掉：那是设计边界，不是 bug。

---

## 6. 该读什么

| 想知道 | 读 |
|---|---|
| 三个回调、描述体、`ctx` 里有什么 | `api/af_api/app.h` 的头注释（**权威**） |
| 资源与配额语义 | `api/af_api/res.h` |
| 服务注册与提交型接口 | `api/af_api/svc.h` |
| 拉起/停止/回收别的 App（管理器） | `api/af_api/mgr.h` |
| 事件 | `api/af_api/evt.h` |
| 最短样板 | `apps/demo/app_demo.c`（82 行） |
| 全局规矩、四行交付、停手条件 | [`AGENTS.md`](../../AGENTS.md) |
| 命令、失败速查 | [`docs/build-and-usage.md`](../../docs/build-and-usage.md) §6/§10 |
