# ESP AI Skill

给**不可信的 AI** 写 ESP32 App 用的框架。

用机器判定代替自我声明：越权、误用 RTOS 原语、在阻塞点后面写清理这三类错误，
让它**编译不过或测试变红**，而不是靠人评审时记着。

**现在不是什么**：不是"ESP32 全系通用框架"的完成态。v1 只有九项基础设施
（内存 / 时钟 / 睡眠 / 互斥 / 建任务 / 退任务 + 表版本 + 能力位 + 日志钩子），
**没有显示屏、输入、音频、网络、存储、电源的能力头**——`api/af_api/` 里只有
`app.h`（生命周期）、`res.h`（资源与配额）、`evt.h`（事件）、`svc.h`（服务）、
`mgr.h`（管理器面：拉起/停止/回收别的 App）。
文档里提到的 `disp_commit` 之类是设计稿，不是可调用的东西。

---

## 为什么存在

嵌入式 + AI 组合最难的地方不是"AI 不会写代码"，是**它写的代码看起来对**：
清理写在阻塞调用后面（那行可能永远不执行）、拿 `xSemaphoreCreateBinary` 当锁、
把栈上的缓冲区交给异步提交。这些错误编译得过、跑一阵也不崩。

所以这个项目的赌注是三条：

| 赌注 | 做法 | 取代了什么 |
|---|---|---|
| **伪阻塞** | 所有等待都走框架，取消位命中就 `fw_run_exit()` **不返回**；清理只能写在 `exit()` 里，框架保证它恰好调一次 | "记得在阻塞点之后别写清理" |
| **编译期隔离** | 依赖闭包门禁：App 的公开头闭包必须落在它自己的 `link` 集合里，越界报错指向撑破闭包的那个头 | "评审时留意分层" |
| **机器判定** | 四行交付（Build / Host / Device / Unverified）+ 六道门禁；`SKIP` 不算通过，退出码 `2` 不算通过 | "AI 说它测过了" |

一句话：**接口在头文件里，判定在门禁里，证据在四行状态里。三者缺一，就等于没做。**

---

## 长什么样

```
apps/          业务 App：只依赖 api/af_api + common/      （tag 2）
  └ framework/ 只有机制：生命周期、伪阻塞、注册表、资源、事件、心跳  （tag 3）
      └ platform/  只有九项：实现 pf_ops_t，别的都不许      （tag 4）
api/af_api/    App 能看见的全部契约（唯一横向通道）
common/        类型、错误码、原子宏——最底层，不认上面任何一层
```

两条硬规矩：

- **框架不知道有哪些 App、也不知道板是谁**。靠两个钩子：`app_manifest_primary()`
  （App 层实现）与 `pf_platform_ops()`（平台实现）。加 App、加板都不改 `framework/`。
- **写权不传递**。能 include 一个头，不等于能 include 它 include 的东西——门禁用
  `clang -M` 取整个闭包来判。

### 六条机制

| 机制 | 一句话 | 关键决定 |
|---|---|---|
| 伪阻塞 + 取消位 | 主任务是 `for (;;) { 干活; fw_delay_block(n); }`，等待被切成 ≤20 ms 的片，每片查取消位 | 退出五步，第一步就是**先清取消位**，否则清理里的阻塞会再次命中 |
| 提交即拷贝 | `fw_svc_submit()` 返回后，调用方缓冲区可以立刻重用 | 零拷贝留作备选：违约是画面撕裂这种**静默**错误，机器抓不到就不换 |
| 服务注册表 | 未注册 `NOT_SUPPORTED`、注册了但崩了 `UNAVAILABLE`、可重试 | owner 死了**保留表项**，否则"崩了"永远退化成"没有"，两个码变成一个 |
| 资源槽位 + 配额 | `res_acquire(&ctx->slots[i], ...)` 把句柄写进上下文；框架在退出第 ③ 步只收 `RES_LIFE_APP` | 超配额是 `QUOTA`（查漏释放）不是 `NOMEM`（整机没了）——两个码说的是两件事 |
| 事件环 | 每 topic 一个无锁环，ISR 版**无长度参数**（长度在声明时定死） | 满时丢新的：丢旧的会让正在处理的事件看到"历史被改写" |
| 心跳 | 框架在让出点自动喂；`fw_app_is_stalled()` 只观测 | 本仓库负责"能被停下来"，产品仓库负责"发现停不下来时重启整机" |

---

## 现在有什么 / 没有什么

**有**（都有 host 用例；多数有两块板的真机证据）：

- 生命周期：EMPTY → INIT → RUNNING → STOPPING → STOPPED / FAILED，`exit()` 恰好一次
- 清单注册与查重、`fw_alloc_id()` 发号（重启必换号）、`fw_boot()` 只拉 SYSTEM App
- 伪阻塞、取消、退出五步、`in_exit` 期间取消失效
- 服务注册表（三码语义 + 认领）、提交型接口（拷贝、同 key 覆盖、表满 `BUSY`）
- 资源槽位、生命周期归属、配额与撤销（只压上限，不杀任务）
- 事件环（任务里调 + `_isr` 版）、心跳与停滞观测
- 整机入口 `app_main()`（失败停在原地等看门狗）与嵌入入口 `fw_embed_start()`（失败返回码）
- 三块平台实现：`host-fake`（POSIX）、`ai-passport-c3`（单核）、`esp32s3-devkit`（双核）

**没有**：

- 任何能力头（显示/输入/音频/存储/网络/电源）——`docs/generated/catalog.md` 的九位能力目前"查得到、调不到"
- 多 App 并存的用例（注册表支持 253 个，但没验过"A 崩了 B 照跑"）
- ISR 版事件在**真中断**里调用过（现在都在任务里调）
- 设备侧的用例超时（板上挂死 = 看门狗复位，打不出 `FAIL` 行）
- S3 那 8 MB PSRAM 的使用（`mem_alloc` 只走内部 RAM）

---

## 30 秒跑起来

```bash
./build.sh run      # 编译全部用例 + 跑六道门禁 + 跑 host 测试（唯一构建入口）
```

一个用例一个二进制，输出固定四种行：`PASS` / `FAIL` / `SKIP` / `SUMMARY`。
**红了先看 `./build.sh` 的退出码**，再看运行输出——编译失败时旧二进制还在，会跑出假绿。

真机（两块板用同一份工程，按 target 自动选端口实现）：

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
cd tests/port/idf
idf.py set-target esp32c3     # 或 esp32s3；切 target 会 fullclean + 重建 sdkconfig
idf.py build
idf.py -p /dev/cu.usbmodem101 flash          # 需要授权；烧录前先杀掉占串口的 python
```

---

## 一个 App 的样子

`apps/demo/app_demo.c`（78 行，含注释）的核心就这些：

```c
static cm_err_t demo_init(app_ctx_t *ctx) { (void)ctx; return CM_OK; }
static void     demo_exit(app_ctx_t *ctx) { (void)ctx; /* 清理只写在这里 */ }
static void     demo_task(app_ctx_t *ctx)
{
    (void)ctx;
    for (;;) {
        /* 干活 */
        fw_delay_block(100u);      /* 一切等待都走框架，取消位才有效 */
    }
}

const app_desc_t app_demo_desc = {
    2u, "demo", demo_init, demo_exit, demo_task,
    APP_FLAG_SYSTEM, APP_STACK_MIN,
};
```

三条形态约束：主任务没有状态机（框架在片边界替你退出）、清理只在 `exit()` 里、
`init()` 只做一次性准备。**不要**写 `if (should_stop) break;`——那等于把取消协议
复制到每个 App 里，正是这套设计要消灭的东西。

---

## 接一块板

只看一个头：`platform/port/pf_ops.h`（九项，每项一段注释）。实现它 + `pf_platform_ops()`
+ `pf_platform_log()`，然后跑 `tests/port/test_port_conformance.c`（host 与真机同一份）。

三个最容易写错的：`sleep_ms(0)` 必须**让出** CPU（`vTaskDelay(0)` 不让出）、
`sleep_ms(n>0)` 的 tick 数必须**向上取整到至少 1**（否则 1–9 ms 变成忙等）、
`task_exit_current()` **不返回**（其后不得放清理）。

---

## 目录与文档

| 想知道 | 去哪 |
|---|---|
| **给 AI 的入口**（规矩、身份、停手条件） | [`AGENTS.md`](AGENTS.md) |
| 命令怎么用、门禁怎么判、接板流程、失败速查 | [`docs/build-and-usage.md`](docs/build-and-usage.md) |
| 九项为什么这么定（逐项语义与理由） | [`docs/pf-ops-decision.md`](docs/pf-ops-decision.md) |
| 错误码谁产出、哪个码该查什么 | [`docs/error-codes.md`](docs/error-codes.md) |
| 验收线 A1–A9（哪些能自动、哪些要人审） | [`docs/acceptance.md`](docs/acceptance.md) |
| 能力位 ↔ 接口头 | [`docs/generated/catalog.md`](docs/generated/catalog.md) |
| 产品仓库怎么把本仓库引进去 | [`docs/cross-repo.md`](docs/cross-repo.md) |
| 决策与施工记录（D1–D59） | [`实施步骤.md`](实施步骤.md) |

谁写哪个目录由 [`policy.txt`](policy.txt) 决定，不是由你声明：

```bash
python3 tools/check_workspace_policy.py --as 2 apps/demo/app_demo.c   # ALLOW/DENY + 理由
```

---

## 当前基线

> 数字的唯一出处是 [`AGENTS.md`](AGENTS.md) §1；这里是写这份介绍时的快照，漂了以那边为准。

- host 用例 **179 条**全绿；六道门禁 PASS（入口唯一 / link 闭包 / 纯 C 边界 / 能力目录 / 服务号 / 格式）
- 真机：ESP32-C3（ai-passport）与 ESP32-S3 devkit 各 **41 条**，`DEVICE_DONE rc=0`
- ASan、TSan 全量 0 报告

---

## 术语

| 词 | 意思 |
|---|---|
| **伪阻塞** | 看起来能阻塞的接口，内部按片等待并响应取消位；命中取消就**不返回** |
| **取消位** | 每个 App 上下文里的一位。框架在阻塞点查它，不在 App 里查 |
| **提交即拷贝** | 提交接口自己复制数据；返回后调用方缓冲区可重用 |
| **服务注册表** | App 之间不链接符号，按 id 找人；`NOT_SUPPORTED` / `UNAVAILABLE` 是两件事 |
| **能力位** | `pf_ops_t.caps()` 报"这块板有什么硬件"；有位 ≠ 有接口可调 |
| **tag** | 分层写权限（1 系统 App / 2 业务 App / 3 框架 / 4 平台 / 5 接口） |
| **nobody** | 考卷类文件（`tools/`、`build.sh`、`policy.txt`、`AGENTS.md`、`skills/`、`.clang-format`）：AI 只读 |
| **四行交付** | Build / Host tests / Device tests / Unverified；缺一段视为没交付 |

---

## 已知边界（写在明面上，不当它能用）

- 所有真机证据只覆盖 **ESP32-C3 与 ESP32-S3**；别的板要自己补端口与实测
- "换板不改框架"由同一个作者走通了一次，**没有**验证过"陌生人只凭头文件也能接"
- 能力的语义（显示怎么刷新、存储怎么分块）**尚未设计**，只有能力位
- 纯死循环的 App 框架发现不了也停不掉：这是设计边界，处置归产品侧（看门狗复位）
