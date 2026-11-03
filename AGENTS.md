# ESP AI Skill —— 给 AI 协作者的入口

**一句话**：这是给"不可信的 AI"写 ESP32 App 用的框架——用机器判定代替自我声明，
让越权、误用 RTOS 原语、在阻塞点后面写清理这类错误**编译不过或测试变红**，
而不是靠评审记着。

**动手前**：先读 §0 的两道门——交清单等批准、方案先过评审。跳过它会返工。

**现在不是什么**：不是"ESP32 全系通用框架"的完成态。v1 的九项基础设施是
内存 / 时钟 / 睡眠 / 互斥 / 建任务 / 退任务 + 表版本 + 能力位 + 日志钩子。

`api/af_api/` 现在有**六个头**：
`app.h`（生命周期 + 使用面）、`res.h`（资源/配额）、`evt.h`（事件）、`svc.h`（服务）、
`cap.h`（能力查询）、`uart.h`（串口）；外加 `mgr.h` 是**控制面**（只有管理器要 include）。

**真正做出来的设备能力只有串口一项**。显示屏、输入、音频、网络、存储、电源都**没有能力头**——
看到文档提到 `disp_commit` 之类，那是设计稿，不是可调用的东西。
加一项能力的完整路径见 `skills/role-api/SKILL.md` §4（四件套：平台 vtable → 板子注册 →
App 门面 → 框架切片实现）。

---

## 0. 动手前的两道门（先读这一节，再碰文件）

**第一道：先交文件清单，等一句明确的批准，再写第一个字节。**

清单一次给全（覆盖整个任务，不要写着写着再补），四行：

1. **要新建 / 要修改的文件**：写完整路径，不写"相关文件"这种话。
2. **每个文件属于哪个 tag、你有没有写权**：附上
   `python3 tools/check_workspace_policy.py --as <你的 tag> <路径>` 的结果。
3. **为什么不能只改现有的**（能不加文件就不加）。
4. **大概多少行、怎么验收**：跑哪条命令、看哪个数字变绿。

清单里出现一条你写不了的路径 → **停，报告**，不要"先写了再说"。

- **批准 = 人明确说"可以/批准/开工"**。任务描述（"把 X 改成 Y"）只说明要做什么，
  不说明可以动手；"这看起来是小事"也不是批准。
- 清单只列**这次必需**的改动。顺手发现的问题单独一行问，不混进被批准的边界里。

**第二道：用户要"方案"时，方案先过评审，过了才动手。**

方案 = 目标 / 改什么 / 不改什么 / 失败模式 / 怎么验。方案如果也要落成文件，
那个文件同样要进第一道门的清单（"我要写 `docs/xxx.md`"也是要批准的改动）。

**打完代码之后**：按 §6 给四行状态，另外必须说清两件事——

- **实现了什么**：对着清单逐条说，哪条没做、为什么；
- **需要你处理什么**：提交、烧录授权、改 `nobody` 里的文件、拍板某个取舍。

---

## 1. 三十秒上手

```bash
./build.sh run     # 唯一构建入口：编译全部用例 + 跑五道门禁 + 跑全部 host 测试
```

当前基线：**179 条 host 用例全绿** + `check_entry_unique` / `link 面` / `纯 C 边界` / `能力目录` / `服务号` 五道门禁 PASS。
真机：两块板（ESP32-C3 `ai-passport`、ESP32-S3 devkit）各跑同一份 **41 条**用例，`DEVICE_DONE rc=0`。

| 想知道 | 去哪 |
|---|---|
| **项目总览（是什么、为什么、现在到哪）** | [`README.md`](README.md)（10 节：三个赌注、六条机制、有什么/没什么、术语、已知边界） |
| **构建框架怎么搭的、命令怎么用** | [`docs/build-and-usage.md`](docs/build-and-usage.md)（详细版：`build.sh` 行为、五道门禁、写 App、接板、真机流程、失败速查） |
| 一个 App 最短能多短 | `apps/demo/app_demo.c`（78 行，含注释） |
| App 能调什么 | `api/af_api/app.h`（生命周期 + 使用面）、`res.h`、`evt.h`、`svc.h`、`cap.h`、`uart.h`；管理器另加 `mgr.h`（控制面） |
| 板子要实现什么 | `platform/port/pf_ops.h`（九项，每项一段注释，**这是接板唯一必读**） |
| 契约为什么这么定 | `docs/pf-ops-decision.md` |
| **写代码要守什么**（命名/注释/内存/并发） | [`docs/coding-style.md`](docs/coding-style.md)（含 §13「现状与本文不符」，别顺手改） |
| 错误码谁产出 | `docs/error-codes.md` |
| 验收线怎么算过 | `docs/acceptance.md`（A1–A8） |
| 哪个能力位对应哪个头 | `docs/generated/catalog.md`（与 `pf_ops.h` 双向绑定，门禁会红） |
| 产品仓库怎么把本仓库引进去 | `docs/cross-repo.md` |
| 决策与施工记录 | `实施步骤.md`（§7 决策 D1–D59，§8 推进记录） |

---

## 2. 你属于哪种身份（写权由 `policy.txt` 决定，不由你声明）

| 身份 | tag | 典型任务 |
|---|---|---|
| 业务 App | 2 | 写 `apps/<app>/`，追加 `apps/app_manifest.c` |
| 框架 | 3 | 改 `framework/`，补用例 `tests/framework/` |
| 平台 | 4 | 写 `platform/<板>/`，跑 `tests/port/` 契约套件 |
| 接口 | 5 | 定 `api/`，写 `common/`、`docs/` |

判定命令：

```bash
python3 tools/check_workspace_policy.py --as 2 apps/demo/app_demo.c   # ALLOW/DENY + 理由
```

**写权不传递**：能 include 一个头，不等于能 include 它 include 的东西。公开头的依赖闭包
必须落在读者自己的 `link` 集合里（`tools/check_link.py` 用 `clang -M` 判）。

---

## 3. 五条硬规矩

1. **不改 `tools/`、`build.sh`、`.clang-format`、`policy.txt`、`AGENTS.md`、`skills/`**——这六个是考卷本身，归 `nobody`。
   发现它们有问题：**报告，不要自己改**。
   把 `skills/` 也锁上，是因为让 AI 改"给 AI 看的说明书"，它会把约束改写成自己做得到的样子；
   `.clang-format` 锁上，是因为改它等于把全仓的排版 diff 一起改掉。
2. **不跨层**：`apps → framework → platform`；框架不知道有哪些 App、也不知道板是谁
   （靠 `app_manifest_primary()` / `pf_platform_ops()` 两个钩子）。
3. **可阻塞接口一律伪阻塞**：`fw_delay_block()` 之后的代码可能永远不执行。清理只能写在
   `exit()` 里；`exit()` 里允许再调阻塞（`in_exit` 期间取消位失效）。
4. **跨任务状态不许用 `volatile`**：它只挡编译器优化，不给原子性/happens-before。
   框架与 App 统一用 `common/include/cm_atomic.h` 的 `CM_ATOMIC_*`（观测计数用
   `CM_MEM_RELAXED` 即可，但要明白它**不提供同步语义**）。
5. **内存只从 `fw_` 拿**：`fw_alloc` / `fw_free`，禁止 `malloc` 家族与 STL 默认分配器。
   平台例外：`platform/<板>/` 内部可以调 RTOS/`malloc`（它本来就是适配层）。

---

## 4. 写一个 App（最短路径）

1. 复制 `apps/demo/` 的形状到 `apps/<你的 App>/`：`init` / `exit` / `task` 三个函数 +
   `const app_desc_t <app>_desc`（`id` 用 `fw_alloc_id()` 或手工挑未被占用的 2..254，
   `stack_bytes` 至少 `APP_STACK_MIN`，`APP_FLAG_SYSTEM` 只给必须最先起的 App）。
2. 主任务必须是：

   ```c
   for (;;) {
       /* 干活 */
       fw_delay_block(100u);      /* 一切等待都走框架，于是取消位才有效 */
   }
   ```

   **不要**写状态机去查"我该不该停"——框架会在片边界调 `fw_run_exit()`，不返回。
3. 把描述体地址追加进 `apps/app_manifest.c` 的 `s_apps[]`（这个文件 tag 1 与 tag 2 共写，
   只许追加）。
4. 测试放 `tests/app/`，用 `tests/tinytest.h`；只打印 `PASS/FAIL/SKIP/SUMMARY` 四种行。
   `./build.sh run` 会自动发现 `tests/**/test_*.c`。
5. 做完跑 `./build.sh run`，按第 6 节的四行格式交付。

**资源**：`res_acquire(&ctx->slots[i], RES_KIND_MEM, bytes, RES_LIFE_APP|RES_LIFE_BOOT)`，
框架在退出第 ③ 步只回收 `RES_LIFE_APP`；`RES_LIFE_BOOT` 是"借整机的"，自己还。

**配额**：`fw_res_set_quota(ctx, bytes)`（0 = 不限）。超了是 `CM_ERR_QUOTA`（查你自己漏没漏释放），
不是 `CM_ERR_NOMEM`（那是整机没了）。

**把它做成服务**（别的 App 要按名字找你，不能链接你）：

```c
static cm_err_t init(app_ctx_t *ctx) { (void)ctx; return fw_svc_register(SVC_STORAGE); }
static void exit_(app_ctx_t *ctx)    { (void)ctx; (void)fw_svc_unregister(SVC_STORAGE); }
```

调用方先 `fw_svc_ready(id)`，拿 `CM_OK` 再调；`CM_ERR_UNAVAILABLE` = 对方崩了/在重启，可以重试；
`CM_ERR_NOT_SUPPORTED` = 这块产品没有这个能力，重试没意义。跨任务传数据用 `fw_svc_submit()`
（**提交即拷贝**，返回后你的缓冲区就能重用；表满返回 `CM_ERR_BUSY`，不是 `NOMEM`）。

---

## 5. 接一块板

1. 写 `platform/<板>/pf_espidf.c`（或任意名字），实现 `pf_ops_t` 的九项 + `pf_platform_ops()`
   + `pf_platform_log()`。**只看 `platform/port/pf_ops.h`**，不要读 `framework/`。
2. 用 `tests/port/test_port_conformance.c` 验语义（host 与真机同一份）。
3. 九项里最容易写错的三个：
   - `sleep_ms(0)` 必须**让出** CPU（FreeRTOS 上 `vTaskDelay(0)` 不让出，用 `taskYIELD()`）；
   - `sleep_ms(n>0)` 的 tick 数必须**向上取整到至少 1**（100 Hz 下 1–9 ms 会截成 0 → 忙等）；
   - `task_exit_current()` **不返回**（`vTaskDelete(NULL)`），调用方不得在其后放清理。
4. 自检工程：`tests/port/idf/`，按 `IDF_TARGET` 自动选端口；`idf.py set-target esp32c3|esp32s3`。

---

## 6. 什么算"做完了"

```text
Build:        PASS / FAIL / NOT RUN
Host tests:   PASS / FAIL / NOT RUN（贴数字）
Device tests: PASS / FAIL / NOT RUN（没做就写 NOT RUN）
Unverified:   还有什么没验证、为什么
```

四行之外，再写两句：**实现了什么**（对着 §0 的清单逐条）、**需要用户处理什么**（提交/授权/改考卷/拍板）。

- **`SKIP` 不是通过，退出码 `2` 不是通过**（2 = 环境或用法问题）。
- 编译通过 ≠ 硬件验证通过；设备连着 ≠ 获得烧录授权。
- 改代码就补测试；测试红了改代码，不改断言。
- 用真实内存/栈/动态分配要单列一行：申请什么 / 多大 / 谁释放 / 何时释放 / 失败怎么办。

---

## 7. 只有真机才会暴露的坑（都踩过）

| 现象 | 原因 | 结论 |
|---|---|---|
| `Guru Meditation ... Stack protection fault`，只用一行 `fw_log` | `APP_STACK_MIN` 曾定 1024，`fw_vlog` 的 256 字节栈缓冲 + newlib `vsnprintf` 不够 | 现在是 **3072**（实测 2048 勉强活、1024 必死） |
| `FreeRTOS Task "x" should not return, Aborting now!` | 任务函数直接 return | 结尾 `vTaskDelete(NULL)`；host 上 pthread 返回是正常的，所以这条**只有板上能发现** |
| 串口只剩 `task_wdt: Task watchdog got triggered`，一条 PASS 都没有 | 高优先级任务 100% 占 CPU，把测试主任务饿死 | 观测者优先级不能被压过；"纯死循环"的真实表现就是"整机无法被观测" |
| 数据竞争（TSan 报一堆） | 跨任务字段用 `volatile` | 一律 `CM_ATOMIC_*`；状态位变终态 ≠ 任务已退出 |
| 提交表满返回 `NOMEM` | 语义错，会让人去查不存在的内存泄漏 | 是 `CM_ERR_BUSY`（"等会儿再来"） |
| `assert failed: xTaskCreatePinnedToCore ... xCoreID < 1`，然后复位 | 把任务写死绑到 core 1，而 ESP32-C3 是**单核** | 核号按 `portNUM_PROCESSORS` 自适应；host 上没有核的概念，所以这条**只有单核板能发现** |

---

## 8. 什么时候停

命中任一条，**停下来报告**（格式：我在做什么 / 撞到哪条 / 为什么不能自己解决 / 选项与代价 / 我建议哪个 / 回复前不动哪些文件）：

- 要改 `tools/`、`build.sh`、`.clang-format`、`policy.txt`、`AGENTS.md`、`skills/`；
- 要改分层方向或 `api/` 的公开签名；
- 需要某个能力但 `api/` 里没有（例如显示）——报告，别自己造；
- 要烧录真机、要提交、要动别人的仓库；
- 发现文档与实现不一致——先报告，由人决定改哪边。

---

## 9. 一句话记住

**接口在头文件里，判定在门禁里，证据在四行状态里。** 三者缺一，就等于没做。
