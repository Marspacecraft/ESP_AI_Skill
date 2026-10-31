# 测试框架（v1.0）

> **状态：定稿 v1.0（2026-10-30）**
>
> 这一份定的是**测试怎么组织、怎么跑、什么算通过**。具体有哪些测试、还差哪些，
> 记在 [test-plan.txt](test-plan.txt)。改本文要走评审（README 规则 9）。

---

## 1. 四条硬约定

1. **零依赖。** 只用 `cc`（或 `gcc`/`clang`）和 Python 标准库。不引 Unity、CMocka、
   GoogleTest，也不引 CTest。理由：这个仓库的读者是 AI，第三方框架的文档不在它手上，
   它会把 API 猜错；而一个 200 行的断言头，它读一遍就能正确使用。
2. **一个用例程序 = 一个进程，带超时。** 生命周期测试会故意让任务不返回
   （`task_exit_current` 之后那段代码可能永远不执行），同进程跑会拖死整批。
   默认 20 秒，超时判失败。
3. **输出契约只有四种行。** 任何测试程序（C 或 Python）只打印：

   ```
   PASS <用例名>
   FAIL <用例名> -- <一句话说明> | <file>:<line>
   SKIP <用例名> -- <原因>
   SUMMARY pass=N fail=M skip=K <程序名>
   ```

   解析器只认这四种。多了不报错但没用，少了判失败——`测试跑完了但没打印任何结果`
   和 `测试没通过` 在观察者眼里必须一样，否则"没有输出"会被误当成"没问题"。
4. **同一份套件跑两处。** 平台一致性套件 `tests/port/pf_conformance.c` 不依赖
   `framework/`、也不依赖 `api/`，在 host 上一次、编进固件在真机上再一次。
   一份代码两处跑，才谈得上"host 过了、板子大概率也对"。

## 2. 目录：镜像源码分层

```
tests/
├─ test.py              唯一入口
├─ tinytest.h           C 断言头
├─ tinytest.py          Python 版（同一契约）
├─ support/             夹具：假平台、虚拟时钟（不是测试用例，不被 test.py 当层）
├─ api/                 对应 api/
│   └─ common/          对应 api/common/（`test.py api common` 就跑这里）
├─ common/              对应 common/
├─ framework/           对应 framework/
├─ app/                 对应 apps/demo/（业务 App 的测试）
├─ sys/                 整体系统测试（跨层，v1 六条验收在这里）
├─ platform/            对应 platform/（板级实现的 host 侧测试）
├─ port/                平台一致性套件（不是单测，是验收工具）
├─ tools/               门禁自测（测 tools/ 本身）
└─ baseline/            基线快照对比（tag 5）
```

**`tests/<层>/<子路径>` ↔ 源码 `<层>/<子路径>`**：`api/common/cm_list.c` 的测试放
`tests/api/common/test_*.c`。所以 `python tests/test.py api common` 就是"api 下的 common 用例"。

`sys/` 是唯一例外：它不是某一层的镜像，而是**整体系统测试**——跨层、要拉起多个假 App。

## 3. 入口与详细度

```bash
python tests/test.py                # 全部层，只统计
python tests/test.py api            # 整层，只统计
python tests/test.py api common     # 收窄到子目录 → 逐用例打印
python tests/test.py tools gates    # 按名字收窄 → 逐用例打印
python tests/test.py -q api         # 强制只统计
python tests/test.py --list         # 只列要跑什么
```

**规则：收窄到子路径就详细，只给一层就只统计。** 跑某个身份的全部用例时，
逐条打印只是噪音；而一旦收窄，你要的就是"哪一条红了、为什么"。

`tools/run_tests.py` 保留为转发壳，行为与上面完全一致。

**退出码**：`0` 全通过 / `1` 有失败 / `2` 环境问题（找不到编译器、入口不存在）。
`SKIP` 不影响退出码，但**`SKIP` 不是通过**——它只说明"这一段现在没法测"。

`tests/tools/test_gates.py` 是全仓第一个真绿的测试：它测门禁本身（谁能写哪个路径、
退出码是什么），12 条用例，不依赖任何业务代码。

## 4. 断言 API

C（`tests/tinytest.h`）：

| 用法 | 说明 |
|---|---|
| `TT_PROGRAM("名字")` | 放 `main()` 里，启动一组用例 |
| `TT_CASE(用例名)` | 展开成 `static void 用例名(void)`，函数体就是用例 |
| `TT_RUN(用例名)` | 跑这条；失败不中断后面的 |
| `TT_ASSERT(cond)` / `TT_ASSERT_INT(期望, 实际)` / `TT_ASSERT_STR(期望, 实际)` / `TT_ASSERT_NOT_NULL(p)` | 断言失败 = 本用例 `return`（不级联） |
| `TT_FAIL("说明")` / `TT_SKIP("原因")` | 主动失败 / 主动跳过 |

Python（`tests/tinytest.py`）同一套语义：`@case` 装饰器注册，`run("程序名")` 执行，
断言工具 `true/eq/contains/fail/skip`，`sh("工具", "参数")` 跑子命令。

每条输出都 `flush`：程序被杀掉时，已经跑过的用例结果还在。

## 5. 链接什么，由谁决定

**每一层编测试时链接哪些源码，只在 `tests/test.py` 的 `LINK` 表里定义一次。**

| 层 | 链接 |
|---|---|
| `api` | `common/` `api/` |
| `common` | `common/` |
| `framework` | `common/` `api/` `framework/` |
| `app` | `common/` `api/` `apps/demo/` |
| `sys` | `common/` `api/` `framework/` `apps/system/` |
| `platform` | `common/` `platform/` |
| `port` | **不链接任何项目源码**——它必须能独立编进固件 |
| `tools` | 不编 C，只跑 Python |

理由：如果"链接什么"写在测试文件里，被测方就能改"测什么"。放在入口一处，
改它要走评审。

**夹具不进 `policy.txt`**：`tests/support/` 里是假平台（`pf_ops_t` 的桩、虚拟时钟），
它跟着测试一起演进，不需要为它单独开一条写权。谁写测试谁写夹具。

## 6. 虚拟时钟：怎么测"< 500ms"

生命周期有一条验收是"卡在 `fw_delay_block(200)` 的 App，从置取消位到 `STOPPED`
不超过 500ms"。**真睡眠会让这条测试本身就要跑 200ms 以上，而且精度取决于机器负载。**

所以夹具里给一份 `pf_ops_t`：`clock_ms()` 返回测试控制的虚拟时间，`sleep_ms(n)`
立即返回并把虚拟时钟推 `n` 毫秒。于是"停止时延"变成一个可精确断言的数。

**测试不链接 `platform/host-fake/`。** 那是被测对象，不能既当考生又当裁判——
测虚拟平台自己的行为用 `tests/platform/`，测框架逻辑用夹具。

同样的道理，`pf_rtos_t` 在夹具里也是一份最小实现（`delay_ms` 推进虚拟时钟、
`task_create` 记下任务体但不真起线程；需要真并发的那几条用例单独标 `SKIP`）。

## 7. 读不到依赖时：`SKIP` 而不是伪造

测试文件头可以写：

```c
// REQUIRES: framework/include/fw_app.h
```

```python
# REQUIRES: platform/port/pf_ops.h
```

依赖不存在 → 这个程序判 `SKIP`，理由是"依赖 … 还不存在"。这比"假装通过"诚实，
也比"编译失败"有用：编译失败会被当成代码写错，而实际上只是还没到那一步。

## 8. 真机

**同一份 `tests/port/pf_conformance.c` 编进固件跑一次。** 它只依赖 `platform/port/`，
输出还是那四种行，串口打出来即可。

真机侧能查什么、不能查什么写在套件自己的注释里，**不要在这里重抄一遍**：
32 位回绕、ISR 安全、栈深与优先级、真实时延这类只能评审的项，套件不假装能测。

烧录要人来做——**连着设备不等于有烧录授权**（`AGENTS.md` §4）。
