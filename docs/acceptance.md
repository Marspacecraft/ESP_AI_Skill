# 验收线（可执行）

旧仓库的 `docs/app-model.md §7` 曾经列过"v1 六条验收"，但那份文档随旧仓库一起删了，
**原文已经不可考**。与其凭记忆复述一个可能已经走样的六条，不如把"能不能验收"这件事
重新写成可执行的形式：每一条都必须给出**今天就能跑的命令**，或者明确写出"为什么现在
跑不了、缺什么"。不可执行的验收等于没有。

三档验收类型（与 `实施步骤.md` 一致）：`host` 本机可跑 · `真机` 要板子+烧录授权 · `人审` 只能人判断。

| # | 验收线 | 命令 | 现在的结果 | 类型 |
|---|---|---|---|---|
| A1 | **App 的编译单元里不存在平台表**：App 能 link 的每个头，其依赖闭包都落在 App 自己的 `link` 集合内 | `python3 tools/check_link.py` | PASS（5 个 tag / 14 个对外头） | host |
| A2 | **整机只有一个入口**：全仓除 `framework/src/fw_main.c` 外没有第二处 `app_main` | `tests/tools/check_entry_unique.sh` | PASS | host |
| A3 | **清单即授权**：没写进清单的 App 编译好了也不会启动；重复 id 整机不启动 | `./build.sh run`（`test_fw_registry` / `test_demo_lifecycle`） | 154 条全绿 | host |
| A4 | **停止是可完成的**：取消位在片边界被响应（切片 ≤20ms），`exit()` 恰好一次，`fw_delay_block()` 之后的代码不执行 | `./build.sh run`（`test_fw_delay` 7 条）+ 真机 `test_app_behavior` | 全绿；真机 `cancel_stops_the_task_and_calls_exit_exactly_once` / `long_delay_is_cut_into_slices` PASS | host + 真机 |
| A5 | **退出路径按生命周期回收**：`RES_LIFE_APP` 槽位被回收、`RES_LIFE_BOOT` 留着 | `./build.sh run`（`test_res`）+ 真机 `test_res_on_device` 8 条 | 全绿；真机验到 `RES_LIFE_APP` 退出回收 2 个槽位、`RES_LIFE_BOOT` 存活、`CM_ERR_QUOTA` 与 `NOMEM` 不混、`fw_res_revoke` 把上限压到当前用量且不杀任务 | host + 真机 |
| A6 | **越权在合并前就红**：以某个 tag 的身份写别人的路径必须被拒 | `python3 tools/check_workspace_policy.py --as 2 framework/src/fw_app.c`（期望 `DENY`、退出码 1） | 可执行 | host |
| A7 | **未验证无法伪装成已验证**：交付必须带四行状态，`Device tests:` 不许在没烧录时写 PASS | 人审 | 无机器判定 | 人审 |
| A8 | **换一块板不改框架**：新增 `platform/<板>/` 只动该目录 + 一处 `pf_platform_ops()` | 人审 + `真机` | 两块板各走通一次：`platform/ai-passport-c3/`（产品固件内嵌）与 `platform/esp32s3-devkit/`（全程未改 `framework/`） | 人审/真机 |

## 三处已知的"还不能自动化"

1. **A3 的"编译好了也不会启动"只验了一半**：`fw_init()` 的清单合并与 id 查重是 host 可验的；
   但"编译进固件却不进清单"这件事，host 上看不到链接结果——它要等目标构建（阶段 9.2 已接通）
   给出符号表才能判。目前靠清单只有一处 `apps/app_manifest.c` 这一结构保证。
2. **A7 没有机器判定**。四行状态是格式，不是证据；能自动化的只有"`Device tests: PASS` 时必须附
   串口摘要"这一类弱检查，且容易被伪造。这条留给评审。
3. **A8 走通了两块板，但都是同一个人、同一份契约的延伸**。`ai-passport-c3` 与
   `esp32s3-devkit` 都由本仓库作者写，且九项语义直接照 `pf_ops.h` 的注释落——所以它证明的是
   "**换板不需要改框架**"，**没有**证明"别人只凭头文件也能接一块板"。后者要等一块
   作者没看过的板（或一个没读过本仓库的 agent）来验。

## 怎么用

- 改完任何一处，先跑 `./build.sh run`：它已经把 A2、A1 接在测试之后（`check_entry_unique` → `check_link`），
  再接 `host` 用例。红一项就整轮红。
- 交报告时按 A1–A8 逐条写"验了 / 没验 / 为什么"，比写"测试通过"有用得多。
