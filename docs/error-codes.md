# 错误码归属表

一个错误码的含义，不能靠"看名字猜"。这张表回答三个问题：**谁产出它**、**在什么情况下**、**调用方该做什么**。

规则（与 `common/include/cm_types.h` 的注释一致）：

1. **数值即契约**，只增不改、不复用、不重排（日志、测试、跨仓库比对都靠它）。
2. **平台层不产出 `cm_err_t`**。`pf_ops_t` 的九项用 `NULL` / `-1` / `0` 表达失败（见 `platform/port/pf_ops.h`），翻译成错误码是框架的事。平台若开始返回 `cm_err_t`，说明它抄了近路，依赖方向也反了。
3. 同一个失败**只有一个码**。"合成一个码"的地方，现场就只能靠猜——`CM_ERR_QUOTA` 与 `CM_ERR_NOMEM` 分开就是这条规则的产物。

| 码 | 值 | 谁产出 | 什么情况下 | 调用方该做什么 |
|---|---|---|---|---|
| `CM_OK` | 0 | 全部 | 成功 | 继续 |
| `CM_ERR_INVALID` | 1 | `fw_ops_check` / `fw_registry` / `fw_res` / `fw_event` / `fw_submit` / `fw_quota` / `fw_app_ctx_new_with_id` / `fw_embed` | 参数非法（NULL、长度越界、槽位不属于本实例、`bytes==0`）、清单条目缺字段、`flags` 含未知位 | 改调用，不要重试 |
| `CM_ERR_NOMEM` | 2 | `fw_quota`（`alloc_impl`）/ `fw_alloc_id` / `fw_registry`（容量满） | 整机内存给了但没拿到；发号器 253 个用尽 | 省着用、释放后再试；不是"你用超了" |
| `CM_ERR_NOT_SUPPORTED` | 3 | `fw_svc_ready` / `fw_svc_owner` / `fw_event_*` / `fw_svc_submit_take` | 服务没注册（这块板没有）、事件主题没声明、提交队列已空 | **不重试**；这是合法状态，不是故障 |
| `CM_ERR_UNAVAILABLE` | 4 | `fw_svc_ready` / `fw_svc_owner` | 服务注册过但持有者已死（STOPPING/STOPPED/FAILED） | 可重试（等它被重启或认领） |
| `CM_ERR_VERSION` | 5 | `fw_ops_check` | `pf_ops_t.version` 与 `PF_OPS_VERSION` 不符 | 启动期拒；**不要试图兼容**，重编平台 |
| `CM_ERR_TIMEOUT` | 6 | `fw_event_take` / `fw_event_wait` | 带超时的等待到点仍未拿到；`take` 时队列为空 | 正常路径，不是错误；继续干活 |
| `CM_ERR_BUSY` | 7 | `fw_res`（槽位已占）/ `fw_event`（队列满、重复 declare）/ `fw_submit`（try-lock 失败）/ `fw_svc_register`（被别人占） | 同名资源正被占用；语义是"现在不行"，不是"永远不行" | 稍后再试；不要去抢 |
| `CM_ERR_STATE` | 8 | `fw_registry`（未绑表、重复 init）/ `fw_boot`（未 init、二次 boot）/ `fw_res`（不在 App 任务里）/ `fw_embed`（二次嵌入、平台无表） | 当前状态机不允许这个动作 | 检查调用顺序，改代码；重试无用 |
| `CM_ERR_CANCELED` | 9 | **目前无产出点（预留）** | 保留号码：将来若出现"被停止时还要早退"的场景再给它产出者 | **不要拿它做分支**；清理写在 `exit()` 里 |
| `CM_ERR_QUOTA` | 10 | `fw_quota`（`alloc_impl`）/ `fw_res`（`res_acquire` 透传） | 本 App 的配额用超了 | 查自己有没有漏释放；别怪整机 |

## 一个已定案的空位，一个未拆的歧义

### `CM_ERR_CANCELED` 是预留值（2026-11-03 定，选项 A）

取消的语义是**"不再返回"**，不是"返回一个码"。取消位命中时，`fw_delay_block()` 直接走
`fw_run_exit()`（清位 → `exit()` → 回收 → 结束任务），调用点根本执行不到，所以 App
永远拿不到这个码。

保留 9 的理由只有一条：错误码只增不改，先占住号，将来真有"被停止时要早退"的场景
（例如 `res_acquire` 在取消位已置时快速失败）再给它产出者，不必占新号。

**在有人给它产出者之前，谁写 `if (rc == CM_ERR_CANCELED)` 谁就是在写死代码**——
清理逻辑只能写在 `exit()` 里，不能写成"检查返回值"。被否决的备选：立刻给
`res_acquire`/`fw_svc_submit` 加"取消位已置就返回它"（B）——那会造出第二套
"怎么发现我被停了"的语义；直接删掉（C）——违背"只增不改"，且将来要占新号。

### `CM_ERR_BUSY` 压了两层含义

在 `fw_submit` 里是"锁被占"，在别处是"槽位/队列被占"。压在一起是因为它们对调用方的
建议完全一样（稍后再试）；如果将来某处需要区分，**拆新码，不改旧义**。

## 怎么验这张表没漂

```bash
# 每个码在代码里的产出点数（`CANCELED` 只有字符串那一处，正是"预留值"的证据）
for e in INVALID NOMEM NOT_SUPPORTED UNAVAILABLE VERSION TIMEOUT BUSY STATE CANCELED QUOTA; do
    printf '%-16s %s\n' "$e" "$(grep -rn "CM_ERR_$e" --include='*.c' common framework platform api | wc -l)"
done
```

改了产出点就重跑一遍，把数字写进提交说明。表与代码不一致时，**以代码为准**并把表改回来；表不是契约，`cm_types.h` 才是。
