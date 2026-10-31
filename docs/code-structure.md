# 代码结构（v1.0）

> **状态：定稿 v1.0（2026-10-30）**
>
> 目的：先定"哪里放什么"，再定接口细节。
> 相关：[architecture.md](architecture.md) 三层与解耦规则
> 还没落地的工作项与仍开着的议题在 [code-structure.txt](code-structure.txt)。

---

## 1. 顶层

```
esp-ai-skill/                # 项目名 ESP AI Skill；MarsAIReader 是它的一个默认平台，不是仓库名
├─ AGENTS.md                 # 人维护（不在任何 role 行里 → 只有人能写），入口
├─ policy.txt                # 人维护，权限表的唯一机器可读事实
├─ CMakeLists.txt            # 人维护，构建入口
├─ docs/                     # tag 5 写（理由见 §1.1）
│   ├─ README.md             总览与文档纪律（十条规则）
│   ├─ architecture.md       三层、解耦、伪阻塞总纲
│   ├─ app-model.md          App 结构体、生命周期、谁启动谁
│   ├─ code-structure.md     本文件
│   ├─ roles.md              tag / 角色 / 谁能写什么 / 停止条件
│   ├─ access-control.md     三条判定的语义与失败模式
│   ├─ glossary.md           术语（名字、编号、契约词）
│   ├─ sys/                  tag 1 写   系统 App 的设计笔记（管理器行为、各服务 App）
│   ├─ app/                  tag 2 写   业务 App 的设计笔记（你要写阅读器就写这）
│   ├─ framework/            tag 3 写   机制细节（比 architecture.md 更靠实现）
│   ├─ platform/             tag 4 写   板级笔记（引脚、时序、踩过的坑）
│   └─ generated/            tag 5 写   生成物（能力目录、依赖图），由 tools/ 产出，不手改
├─ common/                   tag 5 写   公共基础库 —— 任何层可 include，自身不依赖项目内任何头
│   ├─ include/              cm_port / cm_types / cm_err / cm_atomic / cm_list / cm_ring / cm_bits / cm_crc / cm_tls
│   └─ src/                  少数需要 .c 的实现（字符串表、CRC）
├─ api/                      tag 5 写   接口 —— 只有声明，没有实现
│   ├─ af_api/               App 面向：api.h（版本与能力开关）/ app.h / res.h / cap.h / log.h
│   │                        + disp.h / input.h / audio.h / stor.h / net.h / power.h（未设计）
│   └─ fp_api/               框架↔平台（暂不使用，见 §3 注）
├─ framework/                tag 3 写   框架层 —— 机制，不含业务
fw_log.c / fw_main.c
│                            —— `fw_main.c` 提供 `app_main()`：平台初始化 → `fw_init()` → `fw_boot()`
│                            —— 任务、互斥、延时一律走 `platform/port/pf_rtos.h`
├─ apps/                     子目录按层归位（见 §7）
│   ├─ app_manifest.c/.h     tag 1 + tag 2 写，tag 3 读   App 注册清单（机制见 app-model.md §5）
│   ├─ demo/                 tag 2 写   AI 的样板 App（你唯一能建 App 的地方）
│   └─ system/               tag 1 写   系统 App（**属产品仓库**，本仓库不建）
├─ platform/                 整棵归 tag 5，三个板目录归 tag 4（最长前缀）
│   ├─ port/                 tag 5 写   平台契约头（pf_ops.h）与 RTOS 原语兼容接口（pf_rtos.h）
│   ├─ drivers/              tag 5 写   多平台共用的器件驱动（决定 29）
│   ├─ host-fake/            tag 4 写   host 参考平台：`pf_ops_t` 用真 malloc，`pf_rtos_*` 用 pthread
│   ├─ metalio_eink_4_plus/  tag 4 写   板级实现 = **MarsAIReader**
│   └─ otherplatform/        tag 4 写   其他平台
├─ skills/                   只有人能写（**考卷，AI 不得改**；v1.5 起从 role 5 摘掉）
├─ tools/                    tag 5 写（**临时**）门禁脚本、生成器（纯 Python）
└─ tests/                    整棵归 tag 5，四个子目录归 1/2/3/4
    ├─ test.py               tag 5 写   唯一入口：`python tests/test.py <层> [子路径...]`（见 test-plan.md §3）
    ├─ tinytest.h            tag 5 写   C 断言头：只打印 PASS / FAIL / SKIP / SUMMARY 四种行
    ├─ tinytest.py           tag 5 写   Python 版，同一套契约
    ├─ support/              tag 5 写   夹具：假 `pf_ops_t`、假 `pf_rtos_t`、虚拟时钟（不是层，入口不扫）
    ├─ api/                  tag 5 写   对应 `api/`（`test.py api common` 跑 api 下的 common 用例）
    ├─ common/               tag 5 写   对应 `common/`
    ├─ sys/                  tag 1 写   **整体系统测试**（跨层；v1 生命周期六条在这里）
    ├─ app/                  tag 2 写   业务 App 的测试
    ├─ framework/            tag 3 写   机制的测试
    ├─ platform/             tag 4 写   板级测试（host 侧验 `pf_ops_t` 的实现）
    ├─ tools/                tag 5 写   门禁自测（`test_gates.py`）
    ├─ baseline/             tag 5 写   基线快照对比
    └─ port/                 tag 5 写   平台一致性套件（pf_conformance.c，同一份文件在 host 与真机各跑一次）
```

### 1.1 与 `policy.txt` 的逐行对照

上树每一行都标了归属（`tag N 写` / `人维护`）。这份标注必须与 `policy.txt` 一致——**不一致就是 bug，以 `policy.txt` 为准**（它是唯一机器可读的权限事实，见 [roles.md](roles.md) §1）。

**一个文件可以有多个角色**：`apps/app_manifest.c` / `.h` 由 tag 1 与 tag 2 共写、tag 3 读，靠"同样长度的最长前缀并集放行"实现（`policy.txt` 决定，见 [access-control.md](access-control.md) §2.4 / 决定 16）。

两处容易读错：`platform/` 与 `tests/` 的**整棵**归 tag 5，但其中的板目录归 tag 4、测试子目录归 1/2/3/4——最长前缀截断，`role 5` 只是兜底，不是"tag 5 什么都能改"。仓库根下**新出现的顶层目录不属于任何人**，必须先由人在 `policy.txt` 加一行（见 [access-control.md](access-control.md) §2.3）。

对照靠人读——目前没有脚本核对这棵树与 `policy.txt`（缺口记在 [code-structure.txt](code-structure.txt)）。

---

## 2. 依赖规则（这是本设计真正的约束）

| 目录                                     | 可以 include                                      | 不得 include                    |
| ---------------------------------------- | ------------------------------------------------- | ------------------------------- |
| `common/`                                | 标准 C                                            | **任何**项目内目录              |
| `api/`                                   | 标准 C、`common/`、`api/`                         | framework / platform / apps     |
| `framework/`                             | `api/`、`common/`、`framework/`、`platform/port/` | `apps/`、`platform/` 的实现     |
| `apps/demo/`（AI 写，**本仓库**）        | `api/af_api/`、`common/`、自己                    | framework / platform / 别的 app |
| `apps/system/`（系统 App，**产品仓库**） | 上面 + `framework/include/fw_*.h`                 | 别的 app 的内部头               |
| `platform/`                              | `platform/port/`、`api/fp_api/`、`common/`、平台自有 SDK | framework / `api/af_api/` / apps |
| `tests/`                                 | 全部（白盒）                                      | —                               |

三条由此推出的铁律：

1. **`api/` 是唯一横向通道。** 两个 App 之间不直接链接符号，只能通过 `api/` 的 C 函数。
2. **`platform/` 不认识业务。** 它只 include `platform/port/`、`api/fp_api/` 与 `common/`，看不见 `api/af_api/`——那是 App 面。
3. **`framework/` 不认识具体 App。** 它通过服务注册表拿到函数指针，不 include `apps/`。

> 权限差异是刻意的：AI 写的 App 连 `framework/include` 都拿不到，只能看 `api/`。这与"AI 不得修改应用层以外的代码"是同一件事的两面。

### 2.1 `common/` —— 唯一的"人人可用"目录

`common/` 是依赖图里唯一向所有层开放的目录，因为它**不含任何策略**，只有原语：

| 头文件        | 内容                                                                                             | 为什么必须有                                                                                 |
| ------------- | ------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------- |
| `cm_port.h`   | **标准 C 兼容层**：C11 版本闸门、`CM_INLINE`/`CM_NORETURN`/`CM_RESTRICT`/`CM_STATIC_ASSERT` 等宏 | 工具链差异只在这一处收敛，上层永远只写标准 C                                                 |
| `cm_types.h`  | 定宽别名、`CM_ARRAY_SIZE`/`CM_MIN`/`CM_CONTAINER_OF`/`CM_ALIGN_UP`                               | 这些宏每个文件都在用，各写一份就会各写错一次                                                 |
| `cm_err.h`    | **全项目唯一的错误码枚举**                                                                       | 分层各自定义错误码 → 每次跨层调用都要转换，转换函数默认值会吞掉错误                          |
| `cm_atomic.h` | `cm_atomic_i32/u32/ptr` + 内存序                                                                 | 不用 `<stdatomic.h>`：交叉工具链支持不一致；用 `__atomic_*` 内建保证各目标行为一致           |
| `cm_list.h`   | 侵入式双向链表                                                                                   | MCU 上不能为节点再 malloc 一层；节点本来就嵌在结构体里                                       |
| `cm_ring.h`   | 单生产者单消费者无锁字节环                                                                       | ISR→任务、DMA→任务、AT 响应流全靠它，是最容易写出竞态的地方                                  |
| `cm_bits.h`   | popcount/clz/ctz/next_pow2/位图                                                                  | 1bpp 帧缓冲和位图分配反复用到，手写是 off-by-one 高发区                                      |
| `cm_crc.h`    | CRC-16/CCITT-FALSE、CRC-32/ISO-HDLC（增量式）                                                    | 书库文件、字体包、NVS 记录、AT 帧都需要，算法口径必须统一                                    |
| `cm_tls.h`    | 任务局部变量（8 个槽，值为裸指针）                                                              | 伪阻塞要求"任意深度的阻塞点都知道自己在哪个 App 里"；透传 ctx 会让每个签名多一个参数且容易漏 |

`common/` 的四条禁令：

1. **不分配内存、不阻塞、不关中断、不打日志**（打日志要依赖 `api/log.h`，那就成环了）。
2. **没有需要初始化的全局状态**（没有 `cm_init()`）。有状态的库不是原语，是服务。
3. **不出现业务名词**（没有 `page`、`book`、`asr`）。
4. **不 include 任何项目内头文件**——这是它同时向所有层开放的前提。

> **`cm_tls.h` 是第 2 条的一个受控例外，必须在实现时写清机制。** C11 `_Thread_local` 在裸 FreeRTOS 任务上不是"自动正确"的：FreeRTOS 任务用自己分配的栈、由 `pxPortInitialiseStack` 建立上下文，编译器插的 TLS 初始化代码**不会替新任务跑**。所以：
>
> - **ESP32（IDF）**：用 `freertos` 的 task-local-storage 指针（`pvTaskGetThreadLocalStoragePointer` / `vTaskSetThreadLocalStoragePointer`）实现 `cm_tls_*`，不用编译器 TLS。这属于平台层细节，不得出现在 `common/` 的代码里——`cm_tls.h` 只给声明，实现放在 `platform/<板>/` 或一个具体平台的 `cm_port` 适配处。
> - **host-fake**：用真正的 `_Thread_local`（pthread 环境天然正确）。
> - **谁在工作**：`platform/` 负责让 `cm_tls_set/get` 在同一任务内自洽；`common/` 只保证接口形状。若这条不落实，伪阻塞的"任何深度都知道当前在哪个 App"就是空的。
>
> **v1 定案：只定方案与限制，接口与具体实现后续讨论。**
> **方案**：`cm_tls_set` / `cm_tls_get` 两个函数，8 个槽，值是裸指针、不带所有权；框架固定把 `app_ctx_t *` 放在 0 号槽（其余槽给服务型 App 自用）；作用域是"同一个任务内"。
> **限制**：① **只在任务上下文有效**，ISR 里读到的值未定义，**禁止在中断里用**；② **不跨任务传递**，任务结束即失效，不需要也不允许显式清理；③ **没有槽号注册与冲突检测**，两个模块抢同一个槽不会报错——新增槽位必须先在本节登记，否则就是两个模块静默互相覆盖；④ **平台契约 v1 不含 TLS 项**（`pf_ops_t` 九项里没有它），所以 v1 只能由 tag 4 在自己平台目录里给出实现（理由与缺口见 [code-structure.txt](code-structure.txt)）；⑤ `common/` 不得出现任何平台标识符，`cm_tls.h` 只给声明，实现不在 `common/`。

命名一律 `cm_` 前缀，避免与 ESP-IDF / FreeRTOS 的 `List_t`、`esp_err_t` 等撞名。

原子操作刻意用结构体包装而不是裸 `int32_t`：让"这是原子变量"在代码里一眼可见，防止有人对普通 `int` 做 `++` 然后以为它是原子的。

验收方式：`python tests/test.py`，只依赖 `cc/gcc/clang`，不经过 IDF。

---

### 2.2 纯 C 边界 —— 语言、SDK 与 RTOS 的边界

`common/` `api/` `framework/` 这**三**个目录**只能是标准 C11**；产品仓库的 `apps/` 可以用标准 **C++17**（抽象更容易写）。门禁 `check_purity.py` 把这四个目录一起扫：

- 不 include 芯片 SDK、RTOS、图形库这类**目标平台专有**的头（`esp_log.h`、`driver/gpio.h`、`freertos/FreeRTOS.h`）；
- 不出现它们的标识符与类型（`esp_err_t`、`ESP_OK`、`xSemaphoreHandle`、`vTaskDelay`）；
- 条件编译：`common/` `api/` `framework/` 不写 `#ifdef 某芯片` / `CONFIG_*_TARGET`；**`apps/` 不禁**——`#ifdef` 想禁也禁不干净，越层调用交给符号判定兜底。

**那框架要任务、信号量、队列怎么办？** 它**不直接调 RTOS**，而是调一层"RTOS 原语兼容接口"：`platform/port/pf_rtos.h`——**独立头，里面是一张函数指针表 `pf_rtos_t`**（任务、延时、互斥、信号量、队列，共 18 个成员）。**声明在接口层，实现在平台层**：

- ESP32 侧就是 FreeRTOS 的薄封装（一个 `pf_rtos.c`，每个函数三五行转发）；
- host-fake 侧用 pthread 顶；
- `framework/` 只 include `platform/port/pf_rtos.h`——我们自己的纯 C 头，所以**禁令不必为 RTOS 开口子**。

**约束与 `pf_ops_t` 统一（决定 34）**：都是函数指针表，成员为 NULL 表示不支持，都由 `fw_init()` 校验并返回 `CM_ERR_NOT_SUPPORTED`，失败点在启动而不是链接。

这样三件事同时成立：框架拿得到任务原语、同一份 `framework/` 在 host 上能编能跑、换 RTOS 只改平台目录不动框架。

**C 与 C++ 的交界**：`apps/` 可以是 C++17，而 `common/` 与 `api/` 的头是纯 C。**每个 `cm_*` / `api_*` 头自己带 `extern "C"` 包裹**（`#ifdef __cplusplus` + `extern "C" {` 开头，尾部收口），不让 App 作者记这件事。

等价判据（这是最容易执行的一条）：**把它们和 `platform/host-fake/` 一起编到 PC 上，必须能编译并跑通单测。** 编不过，说明依赖漏进来了。既然 `pf_rtos` 也是平台实现，这条判据顺带证明了它的 host 实现是通的。

编译器与标准库之间的差异全部收敛在 `cm_port.h`：

| 宏                                                          | 作用                        | 为什么需要它                                                                       |
| ----------------------------------------------------------- | --------------------------- | ---------------------------------------------------------------------------------- |
| `CM_INLINE`                                                 | `static inline`             | 头文件里的函数不产生外部符号，多个 TU 都 include 也不冲突                          |
| `CM_NORETURN`                                               | `__attribute__((noreturn))` | 标准 C11 的 `_Noreturn` 只能前置；`void f(void) CM_NORETURN;` 用关键字写会语法错误 |
| `CM_RESTRICT` / `CM_ALIGNOF` / `CM_ALIGNAS`                 | 标准关键字别名              | 上层不必记关键字拼写，换工具链也不用改代码                                         |
| `CM_LIKELY` / `CM_UNLIKELY`                                 | 分支预测提示                | 热路径判空用得上；非 GCC 系工具链直接 `#error`，不假装支持                         |
| `CM_STATIC_ASSERT`                                          | `_Static_assert`            | "假设 u32 是 4 字节"这类错误在编译期就挡住                                         |
| `CM_PACKED` / `CM_WEAK` / `CM_DEPRECATED` / `CM_PRINTF_FMT` | 属性                        | 协议结构体、可替换钩子、格式化参数检查                                             |

**这条规则不靠自觉，靠门禁**：`python tools/check_purity.py`（见 §10）。它扫这四个目录，命中 SDK 头、SDK 标识符、或非法跨层 include 就返回非 0（落地状态见 [code-structure.txt](code-structure.txt)）。

> 为什么必须做成门禁：规则只写在文档里，第一个赶工期的人就会写下 `#include "esp_log.h"`，从此整套代码永久绑死在一个 SDK 上。编译器不会因此报错——它只会在换平台那天一次性爆炸，而那时改动量已经不可收拾。

---

## 3. `api/` —— 应用层可见的全部接口

**是的：App 能看到的全部符号 = `api/af_api/` + `common/include/`。** 没有第二处。

这条同时是个闭合性检查：如果一个 App 需要某个能力而 `api/` 里没有，只有两种可能——要么 `api/` 漏了，要么这个 App 在做不该它做的事。不存在"我自己绕过 api 直接调"这条路。

```
api/
├─ af_api/        App 面向（tag 1、2、3、5 可读）
│  ├─ api.h       版本与编译期能力开关（**不是唯一入口**，按需 include 具体能力头）
│  ├─ app.h       App 骨架：ctx、exit、fw_delay_block
│  ├─ cap.h       cap_available()
│  ├─ res.h       资源登记与申请（res_track / res_acquire / res_release）
│  ├─ disp.h      屏幕（**v1 不发布**，见 §4.1 与 architecture.md §9）
│  ├─ input.h     按键 / 触摸 / 手势
│  ├─ audio.h     播放 / TTS
│  ├─ stor.h      文件
│  ├─ net.h       连接与传输
│  ├─ power.h     电量 / 休眠
│  └─ log.h
└─ fp_api/        框架↔平台（tag 3、4、5 可读）—— **暂不使用**，见下方注
```

**类型与错误码不在 `api/` 里**，在 `common/include/cm_types.h` 与 `cm_err.h`。它们要跨层共享，放 `api/` 任何一支都会造成反向依赖（见 §2.1）。

> **`pf_ops_t` 与 `pf_rtos.h` 的头不在这里**，在 `platform/port/`。理由：App 根本读不到 `platform/`（`policy.txt` 里 `role 2` 没有 `platform/` 前缀，也没有任何 `read platform/... 2`），隔离已经由读权做到，不必把平台契约搬进 `api/`。`api/fp_api/` 目前空着不用（"暂不使用，有需要再说"）；它留给将来"framework 与 platform 要共享、又不该给 App 看"的类型。

> **落地状态**（哪些头有设计、哪些连设计都没有）见 [code-structure.txt](code-structure.txt)。**要区分"目录规划"和"能 include 的头文件"。**

**`af_api/` 与 `fp_api/` 的分界就是"这一层能不能看见"**：AI 的 App 只读 `af_api/`，于是 `pf_ops_t`、`fw_run_exit`、`fw_cancelled` 这些机制词在它的编译单元里根本不存在。这不是保密，是**编译期就不给它一把能用的工具**——比"约定你别用"可靠得多。`common/` 在 `api/` 之外，是因为类型和错误码要跨层共享，放哪一侧都会造成反向依赖。

**只有声明**，没有实现，也没有 `static inline` 业务逻辑（纯类型辅助宏除外）。

### 3.1 使用面 vs 控制面

同一个能力会有两个头，分属两侧，**不能互相替代**：

| 能力 | `api/af_api/`（使用面，App 可见）            | `framework/include/`（控制面，仅系统 App 可见） |
| ---- | -------------------------------------------- | ----------------------------------------------- |
| App  | `fw_delay_block`、`ctx`、`exit`（回调，由框架调） | `fw_app_register`、`fw_app_stop`、取消位置位    |
| 资源 | `res_track`、`res_acquire`                   | `fw_res_set_quota`、`fw_res_revoke`、用量查询   |
| 服务 | `disp_commit`                                | `fw_svc_register`、`fw_svc_pending`             |

判据：**问"我能不能强制别人"——能，是控制面；只是"我想做某事"——是使用面。**

AI 写的 App 只拿到左列。右列能停别人的任务、改别人的配额，所以只给管理器（id 1）与服务宿主。

每个头里，每个函数一行注释必须写清 **实现者是 framework 还是哪个 service**（见 §4）。这一行是能力目录的生成源。

---

## 4. 一个 API，两个实现家

这是本设计最容易搞错的地方，必须先说清。

| 类别       | 例子                                                     | 实现位置                                                    | 原因                                 |
| ---------- | -------------------------------------------------------- | ----------------------------------------------------------- | ------------------------------------ |
| **机制型** | `stor_open`、`res_acquire`、`fw_delay_block`、`cap_available` | `framework/src/`                                            | 需要在阻塞点检查取消位、需要记账     |
| **服务型** | `disp_commit`、`audio_play`、`input_poll`                | 服务 App（`apps/system/display/` 等，**产品仓库**）+ 注册表 | 有独占硬件、有长时序状态、需要活对象 |

服务型的调用链：

```
app → disp_commit()                      [framework/src/dispatch/ 里的转发层]
        ├─ 查服务是否已注册；未注册 → CM_ERR_NOT_SUPPORTED
        ├─ 配额/超时检查
        ├─ 调 svc->ops->commit(...)       [函数指针，指向 display App]
        └─ 返回前检查取消位 → 若已取消，走退出路径
```

**所有 `api/` 函数都经过转发层，哪怕只有三行。** 理由：取消语义、配额语义必须有唯一落点；否则每个 App 都要自己写检查，等于没有约束。

### 4.1 服务注册表的契约（已定）

```c
/* framework/include/fw_svc.h —— 控制面，只有服务 App 调 */
typedef struct {
    cm_err_t (*commit)(const page_t *page);
    void     (*cancel)(disp_token_t tok);
} disp_ops_t;

/* 注册时连"谁提供这个服务"一起记下：svc_id_t 和 app_id_t 是两个不同的域，
   不能互相顶替（旧版这里写成 fw_app_state_of(SVC_DISP)，那是拿服务 id 去查 App 表）。 */
void    fw_svc_register(svc_id_t id, app_id_t owner, const void *ops);
bool    fw_svc_ready(svc_id_t id);
app_id_t fw_svc_owner(svc_id_t id);   /* 未注册返回 0（无效哨兵） */
```

```c
/* framework/src/dispatch/disp.c —— 转发层，三行也写 */
cm_err_t disp_commit(const page_t *page) {
    if (!fw_svc_ready(SVC_DISP))                              return CM_ERR_NOT_SUPPORTED;
    if (fw_app_state_of(fw_svc_owner(SVC_DISP)) != RUNNING)   return CM_ERR_UNAVAILABLE;
    cm_err_t r = disp_ops->commit(page);
    if (fw_cancelled()) fw_run_exit();    /* §3.1：不返回 */
    return r;
}
```

> 上面借屏幕服务说明形态。**屏幕是 v1 之外的单独立项**，所以 `page_t` / `disp_token_t` 这些类型目前不存在，`api/af_api/disp.h` 也不会在屏幕设计定稿前发布（见 architecture.md §9）。要看形态，看这几行；要看类型，等屏幕。**错误码用的是全项目唯一的 `cm_err_t`**（`common/include/cm_err.h`），不存在 `disp_err_t`。

三种状态必须分开，**不能混成一个错误码**：

| 状态                                | 返回                    | 上层该做什么           |
| ----------------------------------- | ----------------------- | ---------------------- |
| 服务未注册（板上没这能力 / 未启用） | `CM_ERR_NOT_SUPPORTED`  | 隐藏入口，不提示错误   |
| 已注册但 App `FAILED`（服务崩了）   | `CM_ERR_UNAVAILABLE`    | 显示"暂不可用"，可重试 |
| `RUNNING`                           | 正常结果                | —                      |

注册规则：

1. **只注册一次**。重复注册立即报错并拒绝——否则两个 App 抢同一个服务，谁生效取决于启动顺序。
2. **`ops` 必须是 `const`，注册后不可替换**。运行期换实现会让"上一次调用和这一次调用不是同一个东西"。
3. **注册发生在服务 App 的 `init()` 里**，早于任何调用方启动。这条顺序由 `requires` 拓扑排序保证（当前只按清单顺序，见 [code-structure.txt](code-structure.txt)）。把"未注册"与"已注册但崩了"分成两个错误码是必须的：前者被误判成"板上没这能力"会静默隐藏功能。

---

## 5. `framework/` —— 只有机制

```
framework/
├─ include/           fw_*.h：控制面，只给系统 App 用（产品仓库的 apps/system/）
│   ├─ fw_app.h       App 注册、生命周期、取消位、退出路径
│   ├─ fw_svc.h       服务注册表
│   ├─ fw_res.h       记账与限额（对应 res_track / res_acquire）
│   ├─ fw_lock.h      逻辑层锁与租约
│   ├─ fw_event.h     事件总线
│   └─ fw_init.h      fw_init() / 版本校验
├─ src/
│   ├─ app/           状态机、取消位、退出路径、重启
│   ├─ dispatch/      每个 api 函数一个转发（薄，但存在）
│   ├─ res/           记账、ctx 槽位、配额
│   ├─ lock/          逻辑锁、租约、三层冲突的可见层
│   ├─ event/         队列与事件（队列实例属于使用者，不属框架）
│   ├─ svc/           注册表
│   └─ cap/           能力表 + 声明/事实一致性自检
└─ port/              不在这里 —— 见 platform/port/
```

**框架里不许出现的东西**：

- `while(1)` 的任务体、`xTaskCreate`、定时器实例 → 框架没有运行权（§架构 2）
- 任何业务名词（阅读、页面、书架、对话）
- `#ifdef CONFIG_xxx_BOARD` 之类的板级分支
- 对具体器件的直接访问（只经 `pf_*`）

判据：**把 `framework/` 编译到 PC 上、只链接 `platform/host-fake`，必须能跑起来并跑通单测。** 跑不起来说明机制里混进了硬件或业务。

---

## 6. 应用层纯逻辑放哪里 —— 没有 `lib/`

**本仓库没有 `lib/` 这个目录。** 早先设想过一个"应用层公共纯逻辑"层（分页、断行、编码、epub 容器解析），但它定位不清：它和 `common/` 都叫"公共"、都要求纯 C、都能 host 测，区别只剩"谁写它"；而"谁写它"已经由 tag 决定了，不需要再开一个目录来表达。

规则只有一条：**纯逻辑跟它的使用者放在一起。**

| 这段逻辑                                          | 放哪                                            | 谁来写                                    | 理由                                                                                         |
| ------------------------------------------------- | ----------------------------------------------- | ----------------------------------------- | -------------------------------------------------------------------------------------------- |
| 只被一个 App 用（分页、排版、某个格式的解析）     | 该 App 自己的目录，如 `apps/demo/<app>/layout/` | 写得动那个目录的 tag（业务 App 是 tag 2） | 它没有第二个使用者，提前抽出去只会多一层目录和一次跨目录修改                                 |
| 被多个 App 用，且不该让它们各写一份               | `common/`                                       | tag 5（接口层）                           | 跨 App 共享就是"公共契约"的一部分，必须由能改契约的人维护，否则两个 App 会各改各的、悄悄分叉 |
| 被框架和 App 都要用（错误码、定宽类型、原子操作） | `common/`                                       | tag 5                                     | 同上；这也是 `common/` 原本的职责                                                            |

代价是明确的：**AI 想抽公共库，必须提给接口层，不能自己建一个 `lib/` 往里放。** 这是刻意的——一个能被所有 App 引用的目录，等于一个能被所有 App 依赖的公共契约，它的演进必须有单一决策点，而不是"谁先写谁定"。

纯逻辑的验证方式不变：**它不依赖 `framework/`、不开任务、不碰 IO，所以能在 PC 上跑全量单测**；放进 `apps/<app>/` 之后这一点依然成立，`tests/app/` 直接测它。

---

## 7. `apps/` —— 本仓库只放最小可写集

`apps/` 在本仓库里**只包含 AI 的写入目标**，其余（系统 App 与启动策略）属产品仓库。
这条是修订过的：早期文档写的是"`apps/` 在本仓库不存在"，但 `policy.txt` 已经把
`apps/demo/`、`apps/app_manifest.c`、`apps/app_manifest.h` 三条路径给了 tag 2——
路径不存在，AI 就无处落笔，v1 验收线里的生命周期测试也没有驱动对象。
见 app-model.md §9.9（决定 13 已从那张表移出）。

```
apps/                        ← 本仓库只含 demo 与 AI 的清单
├─ app_manifest.c            AI 的清单（**只追加数组末尾一行**）
├─ app_manifest.h            声明 AI 侧描述体符号（`extern const app_desc_t g_app_<名字>;`）
├─ demo/                     AI 的样板 App —— tag 2 可写
│  └─ <app>/                 实现 init / exit / task + 自己的头
└─ system/                   ← 产品仓库，不在本仓库：tag 1 读写；tag 2 **读不到**（`policy.txt` 的 `read apps/system/ 1` 覆盖了 `read apps/`）
   ├─ system_manifest.c      系统 App 的清单
   ├─ manager/               管理器（id 1，文档里的"0 号"）：引导、依赖排序、配额预检、监控、停止
   ├─ display/               显示服务：唯一持有面板句柄 + 刷新计划（屏幕专项另议）
   ├─ input/                 输入服务：按键 / 触摸 / 手势
   ├─ audio/                 音频服务：跨 I2S/UART/GPIO 的会话状态机
   └─ net/                   网络服务：连接、重连、背压
```

**为什么系统 App 不在本仓库**：框架仓库一旦塞进"示例 App"，示例就会变成事实标准——所有
产品照着抄，而真正的管理器策略（优先级、配额、降级）本该各产品自己定。本仓库只保留
一个 `demo/`，它的定位是"能编译、能被测试驱动的最小样板"，不是产品参考实现。

### 清单的形态

`fw_init` / `fw_boot` 接受**一份清单数组 + 数量**，不是单个全局数组：

```c
typedef struct {
    const app_desc_t *const *apps;
    uint32_t count;
} fw_manifest_t;

cm_err_t fw_init(const fw_manifest_t *manifests, uint32_t count);
```

**为什么不是只有一张表**：只有一张时，AI 加自己的 App 就要 `extern` 系统 App 的
`app_desc_t`——AI 的编译单元引用系统 App 的符号，既越权又脆弱（系统 App 改名/挪目录
就断）。支持多份清单后，AI 只看得见自己那份，系统 App 那份由别人维护，框架负责合并。
这条是 `check_policy.py --symbols` 第一次跑就抓出来的，不是设计时想到的
（见 app-model.md §5 与决定 11；决定 13 已移出那张表，见 app-model.md §9.9）。

系统 App 与普通 App 的差别**只在三处**：能否 include `framework/include/`（机制面在这里，
不在 `api/` 里）、能否写 `apps/system/`、它的清单项被谁维护。**没有第四处**。

权限边界由**目录 + `policy.txt`** 表达：`apps/system/` 只给 tag 1 读写，`apps/demo/` 只给 tag 2 读写，两个清单文件由 tag 1 与 tag 2 共写（tag 3 只读——框架要合并清单、查重 id）。

`framework/` 的读权是两段：`framework/include/` 给 `1 3 5`（1 是**唯一的跨层读例外**，管理器必须能调 `fw_app_state_of`），整棵 `framework/` 只给 `3 5`（5 是为了让 `tests/baseline/` 能直接驱动框架）。**tag 2 一段都没有。**

判定细节不在这里写——权威定义是 [access-control.md](access-control.md)。

---

## 8. `platform/` —— 契约 + 共享驱动 + 按板组装

```
platform/
├─ port/                     pf_*.h，契约，只有声明
│
├─ metalio_eink_4_plus/      板级实现：一份完整平台（policy.txt tag 4）。**这一块就是 MarsAIReader**——目录名是板卡型号，MarsAIReader 是产品名（见 [glossary.md](glossary.md)「名字」）
│   ├─ board.yaml            引脚、总线拓扑、器件地址、上电时序参数
│   ├─ pf_ops_impl.c         g_pf_ops 实例
│   ├─ board_init.c          总线 → 器件 → 中断的初始化顺序
│   └─ panel_ed047tc2.c      面板专用传输（并行 8bit）
├─ otherplatform/            其他平台（policy.txt tag 4）
│
└─ host-fake/                参考实现（policy.txt tag 4）：硬必需用真 malloc/pthread，可选能力全缺
```

**新板的落地流程（平台适配者按这个走）**：人在 `policy.txt` 的 `role 4` 行尾追加 `platform/<新板>/` 一个前缀（**不新增 tag**）→ tag 4 自己建目录、填 `pf_ops_t` → `tests/port/` 的一致性套件在 host 上跑绿 → 烧真机按 §9 顺序验能力。**顺序不能反，也不能指望有脚手架**：先加前缀再建目录，否则新目录会先落在接口层名下（见 [roles.md](roles.md) §3.1）；本仓库**没有 `platform/template/`**，实现时参考工程里的墨水屏例程、按契约自主写，不是把一个现成工程拷进来改引脚宏——那是"看起来能跑"，不是"接了进来"。

**板级目录是 `platform/` 的直接子目录，不是 `platform/boards/<板>/`。** 早期文档写成 `platform/boards/esp32s31-metalio/`，与 `policy.txt`、`access-control.md` 都对不上。以 `policy.txt` 为准——它是权限判定的唯一事实源。

写错的后果不是"被判越权"，而是**不报错**：`platform/boards/...` 落在 `role 5` 的 `platform/` 前缀之下，会被静默归给接口层。写代码的人以为自己有权，review 的人以为门禁会拦，两边都不对。

### 8.1 公共驱动 vs 板级目录

|        | `platform/drivers/`（归 tag 5）                       | `platform/<平台>/`               |
| ------ | ----------------------------------------------------- | -------------------------------- |
| 放什么 | 寄存器级读写、内部锁、错误处理                        | 引脚、地址、时序参数、初始化顺序 |
| 依赖   | 只依赖 `port/` 的总线函数（`i2c_transfer`、`gpio_*`） | `port/` + 公共驱动               |
| 可复用 | 多平台共用                                            | 不共用                           |

两个器件（如触摸、PMIC）被两块板共用时，代码只能写一份——否则同一个寄存器的错误会在两个目录里各修一次。所以公共驱动放 **`platform/drivers/`**（决定 29）。**这不需要改 `policy.txt`**：`role 5` 已经写了整个 `platform/`，`drivers/` 自然归接口层。

**驱动不写死引脚和地址**：全部经配置结构体注入。

```c
/* 公共驱动头（位置见上） */
typedef struct {
    int      bus;        /* 哪条 I2C */
    uint8_t  addr;       /* 0x38 */
    int      pin_int;    /* -1 表示轮询 */
    int      pin_rst;
} ft6336u_cfg_t;

cm_err_t ft6336u_init(const ft6336u_cfg_t *cfg);
cm_err_t ft6336u_read(const ft6336u_cfg_t *cfg, touch_point_t *out);
```

板级目录只负责"填这张配置表"。这是公共驱动能跨平台复用的唯一办法，也是避免 `#ifdef BOARD_XXX` 蔓延的唯一办法——**差异是数据，不是条件编译**。

### 8.2 某块板没有的驱动

两种处理，按情况选：

- **能力不存在**（板上就没有这个器件）→ `g_pf_ops` 对应项**填 NULL**（不是 stub 函数，见决定 13）。上层看到 `CM_ERR_NOT_SUPPORTED`，自己降级。
- **器件相同但接法特殊** → 在 `platform/<板>/` 里放该板的实现，**但不靠"同名符号由构建顺序选择"**。

> 上一版这里写"同名符号由构建顺序选择"，那是错的：C 里两个 TU 定义同名外部符号是**重复定义**，行为取决于链接器（多数直接报错，部分静默取第一个），不是可控的选择机制。正确做法二选一：①公共驱动暴露**函数指针表**或 `weak` 默认实现 + 板级覆盖（`CM_WEAK` 已在 `cm_port.h` 里），②公共驱动只提供可组合的小函数，板级自己组装一份。**推荐 ①**，因为"板级覆盖了几个函数"能一眼看出来。

### 8.3 平台层的三条禁令

1. 公共驱动与 `platform/<板>/` **不得 include `api/af_api/` 或 `framework/`**——否则驱动开始认识业务，复用立刻失效。
2. **框架不依赖任何具体驱动或板**。删掉所有板级目录，框架仍应能编译、能链 `host-fake` 跑通单测。
3. 板级目录里不得出现对 App 的引用，也不得知道"这一页该不该重刷"——策略不是平台的事。

> 这三条把"仓库里放了一堆硬件代码"和"框架不针对硬件"这两件事分开了：硬件代码是**可选资产**，框架的编译与测试**不经过它们**。

---

## 9. 构建

| 目标       | 入口                                             | 用途                                                                                |
| ---------- | ------------------------------------------------ | ----------------------------------------------------------------------------------- |
| 固件       | 根 `CMakeLists.txt`（IDF 工程）                  | `idf.py build`                                                                      |
| host 测试  | `python tests/test.py`                           | 按层编译 `tests/<层>/**` 并运行，**不经过 IDF**（规则见 [test-plan.md](test-plan.md)） |
| 单个用例   | `python tests/test.py tools gates`               | 收窄到子路径或名字就逐用例打印；`--list` 只列要跑什么                              |
| 平台一致性 | `python tests/test.py port`                      | 在 host-fake 上跑 `tests/port/`，验九项契约语义                                     |

**这四个入口**（固件、host 测试、单个用例、平台一致性）的落地状态见 [code-structure.txt](code-structure.txt)。

host 构建必须能覆盖 `common/` 与 `framework/` 的全部逻辑。覆盖不到的代码 = 只能在板子上调 = 不该存在（架构 §6 同源判据）。

**`tests/<层>/` 的约定：每个 `test_*.c` 编成一个独立可执行文件，`main()` 里一行 `TT_PROGRAM("名字")` 启动整组用例。**

理由：C 的一个可执行文件只能有一个 `main`。若把所有测试堆进一个程序，一个测试崩溃（段错误 / 死循环）就会让后面的测试全部跑不到，失败清单也只看到第一个。拆开后：加测试 = 加一个文件，不改脚本；一个挂了不影响其他。

**用例表而不是顺序调用**：每个文件用 `TT_CASE(用例名)` 声明用例，末尾 `TT_PROGRAM` 里 `TT_RUN` 逐个跑。这样"只跑一个用例"是运行器能力，不需要为每个测试写第二份开关代码。`--list` 由入口按"文件 + 用例前缀"过滤，不解析源码文本。

**`tests/port/` 不是普通单测，是对平台的验收工具**：它只有 `pf_conformance.c` + `.h`，只 include `platform/port/pf_ops.h`，把一份 `const pf_ops_t *` 当输入、逐项打印 `PASS`/`FAIL`。两种用法：`python tests/test.py port` 在 host-fake 上跑；真机平台把同一个文件编进自己的固件、结果打到串口。**这套东西让"平台适配"有离线反馈回路**，否则九项语义错误只能等设备在现场偶发卡死才暴露。

`apps/` 的编译：本仓库的 `apps/demo/` 与两个清单文件（`apps/app_manifest.c` / `.h`）作为 IDF component 一起编；产品仓库的 `apps/system/` 同法接入。所有 App 编进同一个镜像，由清单在运行时决定启动谁 —— 不做动态加载（架构 §1）。

---

## 10. 门禁（`tools/`）

门禁脚本按"检查什么"分工，每个脚本对应一条可机检的规则：

| 脚本                  | 检查什么                                                                                                                    |
| ------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `check_purity.py`   | §2.2 的语言与 SDK 边界（`common/` `api/` `framework/` 三目录不碰平台专有头、不出现 SDK 与 RTOS 标识符；要 RTOS 原语只能 include `platform/port/pf_rtos.h`；`apps/` 允许 C++17 与条件编译），规则在 `tools/purity_rules.txt` |
| `check_policy.py`   | `policy.txt` 的三条判定，`--as <tag\|human>` 声明身份：①**读权**——真实依赖的头（`clang -M`）必须落在该 tag 的 `read` 集合；②**符号**——`nm -u` 的未定义符号两问：定义在调用者可写区则放行，否则声明必须在调用者不可写的头里且被实际 include；③**写入**——`git diff` 的每个路径必须被该身份的 `role` 前缀覆盖 |
| `check_headers.py`  | 每个头文件能单独 include（自包含）                                                                                                   |
| `tests/test.py`     | host 测试统一入口（`tests/<层>/` 逐个编译运行；收窄即逐用例打印，整层只统计）；`tools/run_tests.py` 是它的转发壳                       |
| `validate.py`       | 一次跑完上面四项，不因前一项失败而中断；`--as` 透传给写入判定                                                                                      |
| `check_layers.py`   | §2 的 include 矩阵（比 purity 更细的分层检查）                                                                                       |
| `gen_catalog.py`    | 从 `api/` + 服务注册生成能力目录                                                                                                   |
| `check_catalog.py`  | 生成物 ↔ 实际注册双向比对，防静默漂移                                                                                                    |
| `check_blocking.py` | 任务体内必须有伪阻塞调用（防死循环）                                                                                                      |

> **落地状态见 [code-structure.txt](code-structure.txt) §10**——在 `check_blocking.py` 与看门狗二者之一落地前，"死循环"没有防线。

**门禁只有一份实现：Python。** 曾经有过 `.sh` + `.ps1` 双份，结论是不划算——规则能同步，行为细节（退出码传播、路径分隔符、找不到编译器时的处置）同步不了，验证成本 ×2 的结果通常只验一个平台。约定：退出码 0 通过 / 1 代码问题 / 2 环境问题；规则数据单独放在 `tools/purity_rules.txt`，便于逐条 review。

> `tools/` 不在 `policy.txt` 的 read 列表里，所以**AI 读不到门禁源码，但要求它能跑**（`python tools/validate.py`）。这不矛盾，但要写清：AI 的依据是**退出码**，不是读规则；退出码 `2` 表示环境坏了，不是通过。

其中 `check_headers.py` 是踩过坑才加的：`cm_ring.h` 曾漏 include `cm_err.h`，而单元测试因为先 include 了 `cm_err.h` 反而看不出来。**只有"单独 include 本头文件"的编译才有机会暴露这类问题**，所以它必须是一项独立检查，不能靠测试顺带覆盖。

---

## 11. 放错地方的信号（自查表）

出现以下情况，说明代码放错了层：

- 某个 `api/` 函数体超过 5 行 → 它应该是一段机制（移到 framework）或一个服务（移到 app）
- `framework/` 里出现 `strstr("chapter")` 之类的业务字符串
- `apps/` 里出现 `pf_` 前缀的符号 → 越了整层
- 两个 App 互相 include → 应该走 `api/`
- `apps/<app>/` 的纯逻辑目录里出现 `vTaskDelay` → 纯逻辑被污染，或它本该是 `common/`
- `platform/` 里出现 `#include "disp.h"` → 平台开始认识业务
- 为一块新板改了 `framework/` → 抽象位置错了
- AI 想新建一个"公共库"目录（原来的 `lib/` 就是这么长出来的）→ 提给接口层放 `common/`，见 §6

---

## 12. 决定记录

> **本节编号是本文内部的局部编号**，不与其它文档共享、不跨文件复用。本节只追加不删除。

| #   | 决定                           | 结论                                                                                                                                                                                                                                                                                                                                     |
| --- | ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | `api/` 按能力拆头              | **已定：拆**                                                                                                                                                                                                                                                                                                                             |
| 2   | 应用层公共纯逻辑放哪           | **已定：不开独立 `lib/`；跟使用者走**——只被一个 App 用的放 `apps/<app>/`，跨 App 共享的提给接口层放 `common/`。见 §6                                                                                                                                                                                                                     |
| 3   | 服务型 API 的实现方式          | **已定：注册表（框架转发）**，见 §4.1                                                                                                                                                                                                                                                                                                    |
| 4   | 公共工具放哪                   | **已定：顶层 `common/`，向所有层开放**，见 §2.1                                                                                                                                                                                                                                                                                          |
| 5   | 错误码是否分层                 | **已定：不分层，全项目一个 `cm_err_t`**，见 §2.1                                                                                                                                                                                                                                                                  |
| 6   | 框架层的可移植边界             | **已定：`common/` `api/` `framework/` 是标准 C11，不 include 平台专有头、不出现 SDK/RTOS 标识符；编译器差异收敛在 `cm_port.h`；由 `tools/check_purity.py` 强制**，见 §2.2。框架需要任务/信号量时走 `platform/port/pf_rtos.h`，不直接调 RTOS（见决定 24） |
| 7   | host 测试的组织方式            | **已定：`tests/<层>/` 镜像源码分层，一个文件一个可执行文件、各自一个 `main()`**（`tests/sys/` 是跨层整体测试），见 §9 与 `docs/test-plan.md`                                                                                                                                                                                                    |
| 8   | 门禁脚本的跨平台形态           | **已定：只用 Python 一份实现，三平台同一份代码；规则数据单独存 `tools/purity_rules.txt`**，见 §10。**已否决**双份（`.sh` + `.ps1`）方案：行为细节同步不了，验证成本 ×2 实际只验一个平台                                                                                                                                                  |
| 9   | 测试用例的选择粒度             | **已定：一个文件里若干 `TT_CASE`，`main()` 里 `TT_PROGRAM` + 逐条 `TT_RUN`**，支持全部 / 某一层 / 收窄到子路径（收窄才逐用例打印）；入口不解析源码，按文件与用例名前缀过滤。见 `docs/test-plan.md`                                                                                                                                                                                                                           |
| 10  | 子进程输出的收集方式           | **已定：写临时文件，不用管道**。本机沙箱禁止命名管道，`capture_output=True` 会 `PermissionError`；采集统一由 `tools/` 里的一个辅助函数做（写临时文件再读回），不在各脚本里各写一份                                                                                                                                                                                       |
| 11  | App 的回调集合                 | **已定：只有 `init` / `exit` / `task` 三个，没有 create/destroy**。exit 覆盖所有结束路径（含 FAILED、含取消），作者没法只在正常路径清理。见 app-model.md                                                                                                                                                                                 |
| 12  | 阻塞接口的形态                 | **已定：一律伪阻塞，App 不检查返回值**。把"停不下来"收敛到框架一处；代价是资源必须登记（见 [code-structure.txt](code-structure.txt) §12）                                                                                                                                                                                                |
| 13  | 平台如何表达"不支持某能力"     | **已定：函数指针为 NULL，不用 stub 函数**。`static inline` stub 在每个 TU 有独立副本，跨 TU 比较函数指针恒不等，能力自检形同虚设；且 stub 制造"看起来实现了"的假象。契约以 `platform/port/pf_ops.h` 为准 |
| 14  | App 的机器标识用字符串还是整数 | **已定：`cm_u8`，`0` = 无效哨兵、`1` = 管理器、`2..254` 归 AI；`name` 保留人读字符串**。id 用于注册表查找、位图、将来的配额；`strcmp` 在每次查找里都是浪费。重复 id 由 `fw_init()` 拒绝启动。日志打 name 不打 id                                                                                                                         |
| 15  | App 机制面放哪                 | **已定：`framework/include/fw_app.h`**（不是 `apps/_system/`）。机制面不放 `api/`：`api/` 对所有 App 开放，把 `fw_run_exit` / `fw_cancelled` 塞进去等于给每个 App 一把停掉自己的刀。系统 App 读 `framework/include/` 是唯一的跨层读例外（`read framework/include/ 1 3 5`）。可见性靠 include 名单 + 门禁，不靠物理目录 |
| 17  | 任务局部变量                   | **已定：`common/include/cm_tls.h`（8 槽，接口形如 `cm_tls_set/get`）**。框架用 0 号槽存 `app_ctx_t*`。放 common 而不是 framework，是因为服务型 App 的实现也要回答"当前是谁在调我"。**实现方式按平台分**：host-fake 用 C11 `_Thread_local`；ESP32 用 FreeRTOS 的 task-local-storage 指针（裸任务的 TLS 初始化不会自动发生，见 §2.1 的注） |
| 18  | 访问权限的表达形式             | **已定：策略表（根目录 `policy.txt`，行格式，角色 × 路径 × r/w）+ 三条判定**，不把规则写死在脚本里。改权限只改数据。格式选行格式而非 TOML/JSON 的理由见 access-control.md §8                                                                                                                                                             |
| 19  | 系统 App 的边界靠目录还是清单  | **已定：目录（`apps/system/`），推翻决定 16 的清单方案**，`apps/system_apps.txt` 取消。理由：写入判定本来就按路径前缀做，路径前缀天然知道 `apps/system/`；清单是第二份要同步的事实。代价必须显式禁止标记 2 写该目录（白名单天然做到）                                                                                                    |
| 20  | 越权怎么拦                     | **已定：三条判定——依赖用 `clang -M` 的真实解析结果、符号用 `nm -u`、写入用 `git diff` 路径前缀**。**已否决**"只扫 `#include` 字面量"：`../` 相对路径、`-I` 顺序、`#include_next`、`__has_include` 都能绕过；**已否决**"只靠 include 白名单"：`extern` 声明即可调用任意符号                                                               |
| 21  | App 注册表一张还是多份         | **已定：多份清单**（`fw_manifest_t` 数组 + count，`fw_init` 合并进内部注册表）。单一表要求 AI 的注册单元 `extern` 系统 App 的 `app_desc_t`，既是越权也脆弱。**这条是符号判定第一次跑就抓出来的**，不是设计时想到的。产品仓库常见形态是"系统清单一份 + 业务清单一份"                                                                      |
| 23  | 符号判定的收紧                 | **两问**：①"这个符号的定义是不是也在可写区"——是则放行（AI 自己的代码，例如它自己的 `g_app_yourname` 和自己的头）；②否则它必须在一个**调用者不可写**的头里被声明。`apps/demo/x/x.h` 是 tag 2 可写的，所以 `void fw_run_exit(void);` 这种"自己造一份权威声明"不算数；`api/`、`platform/port/` 不可写，才作数                              |
| 24  | 框架怎么拿到任务与同步原语     | **已定：不直接调 RTOS。** 走 `platform/port/pf_rtos.h` 这层"RTOS 原语兼容接口"（**18 个** `pf_*` 函数：任务创建与退出、延时、互斥量、信号量、队列），**声明在接口层、实现在平台层**（ESP32 是 FreeRTOS 薄封装，host-fake 用 pthread）。好处：纯度禁令不必为 RTOS 开口子，同一份 framework 在 host 上能编能测，换 RTOS 只改平台目录 |
| 25  | C 与 C++ 的交界                | **已定：每个 `cm_*` / `api_*` 头自己带 `extern "C"` 包裹**（`#ifdef __cplusplus` 开头、尾部收口），不让 App 作者记。`apps/` 可写 C++17，`common/` 与 `api/` 仍是纯 C |
| 26  | `apps/` 的条件编译             | **已定：不禁。** `#ifdef` 想禁也禁不干净；越层调用交给符号判定（`nm -u` 两问）兜底 |
| 27  | `api.h` 是不是唯一入口         | **已定：不是。** App 按需 include 具体能力头（`app.h`、`res.h`…）；`api.h` 只放版本与编译期能力开关 |
| 28  | `api/fp_api/` 现在留不留       | **已定：暂不使用，有需要再说。** 目录已存在且读权 `3 4 5` 已在 `policy.txt`，不放文件不产生代价 |
| 29  | 公共驱动放哪                   | **已定：`platform/drivers/`，归 tag 5。** 多平台共用的器件驱动写一份，板级目录只注入引脚/地址/时序。**不需要改 `policy.txt`**——`role 5` 的 `platform/` 已经覆盖它 |
| 32  | 启动入口叫什么、谁提供        | **已定：`app_main()`，由本仓库提供**（`framework/src/fw_main.c`）。ESP32 侧就是 ESP-IDF 的启动入口；host 侧由测试或参考平台的 `main()` 调同一个函数，保证两边走同一条路 |
| 33  | `fw_manifest_t` 谁拼           | **已定：两种都允许。** `fw_init(const fw_manifest_t *manifests, uint32_t count)`——`count = 1` 时入口自己拼成一个大数组；`count = N` 时按作者各给一份（AI 一份、系统一份），由框架合并 |
| 34  | `pf_rtos` 与 `pf_ops_t` 的约束 | **已定：统一。** `pf_rtos_t` 也是一张函数指针表，成员为 NULL 即不支持，由 `fw_init()` 校验（返回 `CM_ERR_NOT_SUPPORTED`）。失败点在启动，不在链接 |
| 35  | 一致性套件要不要覆盖 `pf_rtos`  | **已定：不覆盖。** `tests/port/` 只验 `pf_ops_t` 的九项 |
| 31  | 启动顺序                       | **已定：平台初始化 → 应用层初始化（合并清单）→ 拉起管理器（id 1）→ 管理器拉起其余 App。** 前两步由入口函数做，第四步是管理器的策略，不在框架里 |

> 编号 **16、22、30** 不在本表：16 已被 19 取代，22 已被 23 修订，30 已被 34 修订（`pf_rtos` 从"符号集 + 链接期失败"改为函数指针表）。三条的原文与为什么被推翻都记在 [code-structure.txt](code-structure.txt) §12。**本表只追加不删除**——被推翻的条目从这里移到那里，编号不复用。

### 12.1 为什么选注册表（裁决记录）

|                     | A 注册表（选中）                    | B App 直接导出符号（否决）              |
| ------------------- | ----------------------------------- | --------------------------------------- |
| 调用链              | `disp_commit()` → 转发层 → 函数指针 | `disp_commit()` 就是 display App 的符号 |
| 取消检查            | 框架统一做，只有一处                | 每个服务自己记得做                      |
| 配额 / 超时         | 框架统一做                          | 无法统一                                |
| 服务未启动 / 已崩溃 | 两种状态可区分                      | 符号存在但内部未就绪 → 每个服务各判一次 |
| 额外开销            | 一次间接调用                        | 无                                      |
| 复杂度              | 要注册表 + 明确的初始化顺序         | 简单                                    |

决定性理由：**伪阻塞（§3.1）的保证必须落在一处**。选 B 就等于把"每个阻塞点都可取消"这个承诺，分散成"每个服务作者各自记得做"——那它就不是保证，是愿望。

唯一的代价是一次间接调用（几十纳秒）。什么时候值得重新考虑 B：当某个服务的调用频率高到间接调用成本可测（例如每帧上千次），此时应优化该服务的接口形态（批量提交），而不是放弃注册表。
