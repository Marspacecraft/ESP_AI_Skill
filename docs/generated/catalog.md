# 能力目录（capability catalog）

**这张表与代码是双向绑定的**：`tools/check_caps.py` 同时读这张表和
`platform/port/pf_ops.h`，任何一边漏写、写错位、或声称了一个不存在的接口头，`./build.sh run` 就红。

它回答一个问题：**板子报的某个位，对应到哪个能力、以及有没有可调用的接口头。**

- 位号与宏定义在 `platform/port/pf_ops.h`（`PF_CAP_*`），**这里只是登记，不是定义**。
- 「接口头」一列写 `—` 表示**该能力可以被查询，但 v1 没有发布接口头**。
  这不是缺陷：`docs/pf-ops-decision.md` 说得很清楚——**查得到硬件 ≠ 有东西可调**。
  反过来，如果这里写了一份头（例如 `api/af_api/disp.h`），而文件不存在，门禁会红。
- 位 9–31 保留，必须读 0；`PF_CAP_RESERVED_MASK` 不是能力，不进这张表。

| bit | 名字 | 宏 | 接口头 | 说明 |
|----:|------|----|--------|------|
| 0 | DISPLAY | `PF_CAP_DISPLAY` | — | 有屏（v1 不发布显示接口） |
| 1 | DISPLAY_PARTIAL | `PF_CAP_DISPLAY_PARTIAL` | — | 屏支持局部刷新；只有置了 DISPLAY 才有意义 |
| 2 | TOUCH | `PF_CAP_TOUCH` | — | 触摸输入 |
| 3 | KEY | `PF_CAP_KEY` | — | 物理按键 |
| 4 | STORAGE | `PF_CAP_STORAGE` | — | 非易失存储 |
| 5 | NET | `PF_CAP_NET` | — | 网络 |
| 6 | AUDIO_OUT | `PF_CAP_AUDIO_OUT` | — | 放音 |
| 7 | AUDIO_IN | `PF_CAP_AUDIO_IN` | — | 录音 |
| 8 | RTC | `PF_CAP_RTC` | — | 实时时钟 |

## 什么时候该改这张表

- **加一个能力位**：先在 `pf_ops.h` 定义宏，再在这张表加一行——两步都做完门禁才绿。
- **发布一个新接口头**：把「接口头」填成 `api/af_api/<name>.h`，并确认标题确实存在。
- **去掉一个能力**：两边同时删；只删一边会被门禁拦下。

## 不在本表里的头

`api/af_api/` 下这几个不是「能力」，是基础设施，因此不需要能力位：

`app.h`（生命周期与使用面）、`res.h`（资源与配额）、`evt.h`（事件）、`svc.h`（服务注册表）。
