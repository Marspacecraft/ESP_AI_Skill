# 验收线（可执行）

旧仓库的 `docs/app-model.md §7` 曾经列过"v1 六条验收"，但那份文档随旧仓库一起删了，
**原文已经不可考**。与其凭记忆复述一个可能已经走样的六条，不如把"能不能验收"这件事
重新写成可执行的形式：每一条都必须给出**今天就能跑的命令**，或者明确写出"为什么现在
跑不了、缺什么"。不可执行的验收等于没有。

三档验收类型（与 `实施步骤.md` 一致）：`host` 本机可跑 · `真机` 要板子+烧录授权 · `人审` 只能人判断。

| # | 验收线 | 命令 | 现在的结果 | 类型 |
|---|---|---|---|---|
| A1 | **App 的编译单元里不存在平台表**：App 能 link 的每个头，其依赖闭包都落在 App 自己的 `link` 集合内 | `python3 tools/check_link.py` | PASS（5 个 tag / 9 个对外头） | host |
| A2 | **整机只有一个入口**：全仓除 `framework/src/fw_main.c` 外没有第二处 `app_main` | `tests/tools/check_entry_unique.sh` | PASS | host |
| A3 | **清单即授权**：没写进清单的 App 编译好了也不会启动；重复 id 整机不启动 | `./build.sh run`（`test_fw_registry` / `test_demo_lifecycle`） | 122 条全绿 | host |
| A4 | **停止是可完成的**：取消位在片边界被响应（切片 ≤20ms），`exit()` 恰好一次，`fw_delay_block()` 之后的代码不执行 | `./build.sh run`（`test_fw_delay` 的 7 条） | 全绿 | host |
| A5 | **退出路径按生命周期回收**：`RES_LIFE_APP` 槽位被回收、`RES_LIFE_BOOT` 留着 | `./build.sh run`（`test_res`） | 全绿 | host |
| A6 | **越权在合并前就红**：以某个 tag 的身份写别人的路径必须被拒 | `python3 tools/check_workspace_policy.py --as 2 framework/src/fw_app.c`（期望 `DENY`、退出码 1） | 可执行 | host |
| A7 | **未验证无法伪装成已验证**：交付必须带四行状态，`Device tests:` 不许在没烧录时写 PASS | 人审 | 无机器判定 | 人审 |
| A8 | **换一块板不改框架**：新增 `platform/<板>/` 只动该目录 + 一处 `pf_platform_ops()` | 人审 + `真机` | 已用 `platform/ai-passport-c3/` 走通一次 | 人审/真机 |

## 三处已知的"还不能自动化"

1. **A3 的"编译好了也不会启动"只验了一半**：`fw_init()` 的清单合并与 id 查重是 host 可验的；
   但"编译进固件却不进清单"这件事，host 上看不到链接结果——它要等目标构建（阶段 9.2 已接通）
   给出符号表才能判。目前靠清单只有一处 `apps/app_manifest.c` 这一结构保证。
2. **A7 没有机器判定**。四行状态是格式，不是证据；能自动化的只有"`Device tests: PASS` 时必须附
   串口摘要"这一类弱检查，且容易被伪造。这条留给评审。
3. **A8 只走通了一次**，且那块板（`ai-passport-c3`）与本仓库的 `platform/port/pf_ops.h` 是同一时间
   写的——"不用读框架"这个结论还没在"别人写的板"上验过。

## 怎么用

- 改完任何一处，先跑 `./build.sh run`：它已经把 A2、A1 接在测试之后（`check_entry_unique` → `check_link`），
  再接 `host` 用例。红一项就整轮红。
- 交报告时按 A1–A8 逐条写"验了 / 没验 / 为什么"，比写"测试通过"有用得多。
