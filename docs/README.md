# docs/ 索引

本目录只放**读者需要照着做或对照判断**的文档。施工过程与决策记录不在这里，在仓库根的
[`实施步骤.md`](../实施步骤.md)（§4 步骤、§7 决策 D1–D43、§8 推进记录）。

| 文件 | 回答什么问题 |
|---|---|
| [`pf-ops-decision.md`](pf-ops-decision.md) | 平台要提供什么：九项端口表的逐条裁决、为什么只有九项、为什么 `pf_rtos.h` 被合并进来 |
| [`error-codes.md`](error-codes.md) | 每个 `cm_err_t` 谁产出、什么情况下、调用方该做什么；`CANCELED` 为什么是预留值 |
| [`acceptance.md`](acceptance.md) | A1–A8 八条验收线：命令是什么、现在跑出什么、哪几条只能人审 |

## 缺的那一份

**"一个 App 怎么写"还没有文档。** 现在能读到的只有 `api/af_api/app.h` 和
`framework/include/fw_app.h` 的注释（生命周期、伪阻塞、退出五步都写在头注释里），
以及 `apps/demo/` 这个样板。这不是设计选择，是还没写——见 `实施步骤.md` §6.7。
