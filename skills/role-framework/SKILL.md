---
name: role-framework
description: 在 ESP AI Skill 仓库里改框架（framework/ 下的启动序列、注册表、伪阻塞、退出路径、服务表、资源配额、事件环、心跳）时使用——用户说"加个机制""这个退出没走完""取消不管用""TSan 报竞争""事件丢了""配额不准"，或你要动 framework/src、framework/include、tests/framework 时先读这份。
---

# 角色 · 框架层（tag 3）

框架**只有机制，没有主人**：它不认识任何具体 App，也不认识任何具体板子。所有"谁"和"哪块板"
都从两个钩子进来（`app_manifest_primary()` / `pf_platform_ops()`）。你写的东西必须让这条线保持为真。

全局规矩（交清单、四行交付、停手）见 [`AGENTS.md`](../../AGENTS.md)。这里只写这一层独有的判断。

---

## 1. 动手前先确认你写得了

```bash
python3 tools/check_workspace_policy.py --as 3 framework/src/fw_ctx.c
# ALLOW  framework/src/fw_ctx.c   role 3：framework/      （退出码 0）

python3 tools/check_workspace_policy.py --as 3 apps/demo/app_demo.c
# DENY   apps/demo/app_demo.c 属于 role 2；--as 3 不在其中   （退出码 1 → 停）
```

你的写区：`framework/`、`tests/framework/`、`docs/framework/`、以及 `framework/include/`
（这一处与 tag 4 共写，因为平台层要 include 框架头）。

**碰不到**：`apps/`、`platform/`、`api/`、`common/`、`tools/`。

## 2. 头文件的三个位置，别放错

| 位置 | 谁能看 | 放什么 |
|---|---|---|
| `api/af_api/*.h` | App、框架、平台 | **公共契约**（`app_ctx`、`app_desc_t`、`fw_delay_block`、`res_*`、`svc_*`、`evt_*`）。定义在 `common/src` 与 `framework/src` 里实现 |
| `framework/include/` | 目前只有一个转发头 `fw_app.h` | App 调框架符号的入口；**不要往这里加新契约**（D44：契约住 `api/af_api/`） |
| `framework/src/fw_internal.h` | 只有框架自己与 host 测试 | 内部面：`fw_init`、`fw_boot`、`fw_live_add`、`fw_run_exit`、`fw_current_ctx`。**管理器面（`fw_alloc_id`、`fw_app_ctx_new*`、`fw_app_start`、`fw_app_state`、`fw_live_*`）已经公开到 `api/af_api/mgr.h`**（D52），不要搬回这里 |

**判据**：App 需要的符号 → `api/af_api/`；只有框架自己用的 → `fw_internal.h`。放错方向会立刻
被 `check_link.py` 抓住（"tag 1/2 的闭包里有 `framework/src/...`"）。

---

## 3. 五条框架级铁律（每条都对应一次真事故）

1. **`exit()` 恰好一次**。用 `CM_ATOMIC_EXCHANGE(&c->exit_calls, 1u, ACQ_REL)` 的返回值当闸门，
   不要 `if (state != STOPPED) { ... state = STOPPED; }`——两条结束路径同时命中会调两次。
2. **先清取消位，再调 `exit()`**。否则清理里任何一次阻塞都会再次命中取消位，永不出来。
3. **`in_exit` 期间取消失效**。清理常要等外设；这期间再被取消就是"清理到一半又结束一次"。
4. **跨任务状态只能用 `CM_ATOMIC_*`**，禁止 `volatile`（它不给原子性、不给 happens-before；
   TSan 曾一次抓出七处）。内存序：状态位 RELEASE/ACQUIRE、计数器 RELAXED、`exit_calls` EXCHANGE。
5. **框架不许 push 给 App**。要通知就走事件环；要停下来就走取消位。`fw_*` 里出现回调调用
   （除了 `desc->init/exit/task`）就是设计走偏了。

---

## 4. 这一层独有的坑

| 现象 | 原因 | 做法 |
|---|---|---|
| 真机 `undefined reference to 'fw_startup'` | `fw_startup()` 曾与 `app_main()` 同住 `fw_main.c`，嵌入方必须排除 `fw_main.c`（撞产品的 `app_main`） | 启动序列独立成 `framework/src/fw_startup.c`（D37） |
| TSan 报 `fw_ops()` 竞争 | 状态位已变终态，但任务还在最后一片 `fw_delay_block()` 里 | **状态位变终态 ≠ 任务已退出**；`s_ops` 读写走 `__atomic` |
| 事件环"永远有货"，测试挂死 | 两个生产者写同一 topic 时 `head` 被回退，`head - tail` 翻转 | `pending = (uint32_t)(h - tl)`；`> FW_EVENT_DEPTH` → `CM_ERR_STATE`；`== DEPTH` 是满、合法（D50） |
| 调用方去查不存在的内存泄漏 | 提交表满返回了 `CM_ERR_NOMEM` | 表满 = `CM_ERR_BUSY`（"服务还没消费，等会儿"）（D41） |
| 服务崩了却返回"没这能力" | 退出时把表项一起删了 | 只清 owner 指针、保留表项 → 死服务回答 `CM_ERR_UNAVAILABLE`（D23） |
| `error: '/*' within block comment [-Werror,-Wcomment]` | 注释里写了 `framework/**` 或路径通配 | 路径一律写成不带 `*` 的形式（这个坑踩了四次） |

---

## 5. 改完必须跑的三件事

```bash
./build.sh run                                          # 179 条 + 五道门禁
CC="cc -fsanitize=thread -g" ./build.sh run             # 竞争（会慢到 ~70s，因为有个纯死循环样本）
CC="cc -fsanitize=address -fno-omit-frame-pointer -g" ./build.sh run
```

- TSan/ASan **必须 0 报告**；红了先看是真问题还是夹具（夹具的跨任务标志也要 `CM_ATOMIC_*`）。
- 单跑一份：`./build/test_fw_delay`——**先 `./build.sh` 重编**，否则跑的是旧二进制（踩过两次，
  会得出"改了没生效"的错觉）。
- 新用例先写成**会失败的版本**跑一遍（红证）。红证时先看 `./build.sh` 的退出码：编译失败时旧
  二进制还在，会跑出假绿。
- 改了 `framework/` 的代码要按"改过就重验"上板：`tests/port/idf` 两块板各 41 条。

---

## 6. 加一个机制时的顺序

1. 先问：这是**框架机制**还是**App 的活**？能写成 App 的就不要进框架（框架每加一条，
   所有 App 都要理解它）。
2. 决定它属于**使用面**（App 调用）还是**控制面**（框架/管理器调用）——判据：能不能强制别人。
   使用面进 `api/af_api/`，控制面进 `fw_internal.h`。
3. 写头注释四段：入参（含边界）/ 返回值（含失败码）/ 线程与中断可用性 / 注意事项。
4. 写实现 + `tests/framework/` 用例 + 红证。
5. 更新 `实施步骤.md` §7 的决策表（一条 D 号，写清"为什么不是另一种做法"）。

---

## 7. 该读什么

| 想知道 | 读 |
|---|---|
| 伪阻塞、取消位、退出五步的来龙去脉 | `docs/build-and-usage.md` §6 + `实施步骤.md` §7（D15/D16/D20） |
| 九项平台契约（你要调它，不能改它） | `platform/port/pf_ops.h` |
| 公共契约的权威定义 | `api/af_api/*.h` |
| 全局规矩、四行交付、停手条件 | [`AGENTS.md`](../../AGENTS.md) |
| 只有真机才暴露的坑 | `AGENTS.md` §7 |
