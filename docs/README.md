# docs/ 索引

项目**总览**（是什么、为什么这么设计、现在有什么、没什么）在仓库根
[`README.md`](../README.md)——第一次接触先读那一份，本目录是往下钻时的对照表。

本目录只放**读者需要照着做或对照判断**的文档。施工过程与决策记录不在这里，在仓库根的
[`实施步骤.md`](../实施步骤.md)（§4 步骤、§7 决策 D1–D51、§8 推进记录）。

| 文件 | 回答什么问题 |
|---|---|
| [`pf-ops-decision.md`](pf-ops-decision.md) | 平台要提供什么：九项端口表的逐条裁决、为什么只有九项、为什么 `pf_rtos.h` 被合并进来 |
| [`error-codes.md`](error-codes.md) | 每个 `cm_err_t` 谁产出、什么情况下、调用方该做什么；`CANCELED` 为什么是预留值 |
| [`acceptance.md`](acceptance.md) | A1–A8 八条验收线：命令是什么、现在跑出什么、哪几条只能人审 |
| [`build-and-usage.md`](build-and-usage.md) | **构建框架与使用说明（详细版）**：`build.sh` 到底做了什么、四道门禁各判什么、怎么写 App、怎么接板、怎么跑真机、红了怎么看 |

## 四份公开契约（`api/af_api/`）

| 头 | 管什么 |
|---|---|
| `app.h` | App 的形状与使用面（三个回调、描述体、清单、`fw_delay_block`/`fw_alloc`/`fw_log`/心跳） |
| `res.h` | 资源槽位（`res_acquire`）与配额（`fw_res_set_quota`） |
| `evt.h` | 事件（`fw_event_post`/`wait`/`take`） |
| `svc.h` | 服务注册表（`fw_svc_register`/`ready`）与提交型接口（`fw_svc_submit`） |

`framework/include/fw_app.h` 只是把 `app.h` 带进来的转发头（D44）。

## 缺的那一份

**"一个 App 怎么写"现在有两处**：仓库根 [`AGENTS.md`](../AGENTS.md) §4 的最短路径
（五步：复制形状 → 主任务 `for (;;) { 干活; fw_delay_block(N); }` → 追加清单 → 补测试 → 四行交付），
和 [`build-and-usage.md`](build-and-usage.md) §6 的展开版（完整骨架 + 六条铁律 + 服务化写法）。生命周期、伪阻塞、退出五步的
**权威细节**在 `framework/include/fw_app.h` 的头注释里——那句话不复制，只链接。

还缺的是"一个真 App"级别的说明：能力头（显示/输入/音频/存储/网络/电源）一个都还没有，
所以现在写出来的 App 只能是"定时器 + 日志"。能力位的现状见 [`generated/catalog.md`](generated/catalog.md)，
范围边界见根 [`README.md`](../README.md) 的「现在有什么 / 没有什么」。
