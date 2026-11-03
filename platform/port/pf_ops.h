/* platform/port/pf_ops.h —— 平台端口契约（全仓唯一一份）。
 *
 * 这是"接一块新板"时唯一必须读的头：一张表、九项机制、外加版本与能力位。
 * 语义裁决见 docs/pf-ops-decision.md；两者冲突时以本头为准，并回去改那份文档。
 *
 * 三条使用规则（违反任何一条，框架与平台就已经不是一个契约了）：
 *   1. 本头**自包含**：只 include <stdint.h> / <stddef.h>，不 include 任何
 *      platform/ 之外的东西，也不 include common/。平台层不认识框架，也不认识公共库。
 *   2. 本头由 tag 5 写；实现方（每块板）只填 pf_ops_t，不改这个头。要改 = 改契约，
 *      先过评审。
 *   3. 九项**全必需**，没有"可选成员"。某一项为 NULL 视为平台违约，
 *      fw_init() 会拒绝启动——不允许"表里缺一格"这种能力差异，能力差异只用
 *      caps() 与服务注册表表达。
 */
#ifndef ESP_AI_SKILL_PF_OPS_H
#define ESP_AI_SKILL_PF_OPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 结构体布局的版本。改了 pf_ops_t 的成员（增删改序）就必须 +1。
 * 不是板子的固件版本；框架只拿它判断"你填的表和我认识的是不是同一张"。 */
#define PF_OPS_VERSION 1u

/* 任务栈的最小字节数。低于这个值 task_create() 必须返回 NULL，
 * 不许悄悄抬到最小栈——栈不够要炸在接板时，不要炸在运行三个月后。 */
#define PF_TASK_MIN_STACK 1024u

/* ── 互斥量与任务句柄：不透明 ──────────────────────────────────────
 * 平台在自己的 .c 里定义 struct pf_mutex / struct pf_task 的实际内容。
 * 框架只持有指针，永远不碰里面的字段——这样换 RTOS 不需要改框架。
 * 生命周期：互斥量活到平台结束（没有 destroy）；任务在 entry 返回或
 * task_exit_current() 时结束（没有 delete）。 */
typedef struct pf_mutex pf_mutex_t;
typedef struct pf_task pf_task_t;

/* ── 能力位 ────────────────────────────────────────────────────────
 * caps() 返回的位图。**查得到硬件 ≠ 有接口可调**：
 *   PF_CAP_DISPLAY 可以是 1，而 api/af_api/disp.h 根本不存在（v1 不含屏幕）。
 * 位 10–31 保留：必须读 0。框架看到保留位非 0 会判平台违约。
 * 位 0–9 已定：0 显示 / 1 局部刷新 / 2 触摸 / 3 按键 / 4 存储 / 5 网络 /
 *             6 放音 / 7 录音 / 8 RTC / 9 串口。
 * 返回 0 完全合法——一块什么都没有的板子也能跑框架与不依赖硬件的 App。 */
#define PF_CAP_DISPLAY (1u << 0)         /* 有屏 */
#define PF_CAP_DISPLAY_PARTIAL (1u << 1) /* 屏支持局部刷新 */
#define PF_CAP_TOUCH (1u << 2)           /* 触摸输入 */
#define PF_CAP_KEY (1u << 3)             /* 物理按键 */
#define PF_CAP_STORAGE (1u << 4)         /* 非易失存储 */
#define PF_CAP_NET (1u << 5)             /* 网络 */
#define PF_CAP_AUDIO_OUT (1u << 6)       /* 放音 */
#define PF_CAP_AUDIO_IN (1u << 7)        /* 录音 */
#define PF_CAP_RTC (1u << 8)             /* 实时时钟 */
#define PF_CAP_UART (1u << 9)            /* 至少一路可用的应用串口（见 pf_uart.h） */
#define PF_CAP_RESERVED_MASK 0xFFFFFC00u /* 位 10–31，必须为 0 */

/* ── 任务描述体 ────────────────────────────────────────────────────
 * 只有四件事。**没有优先级**：框架不设优先级分带，平台的调度策略由平台自己定，
 * 但必须保证所有任务可被抢占（否则 App 里的 fw_delay_block() 会让整机饿死）。 */
typedef struct pf_task_desc {
    const char *name;         /* 只用于诊断；平台可截断；框架不依赖内容（可为 NULL） */
    void (*entry)(void *arg); /* 任务入口；返回即等于任务结束 */
    void *arg;                /* 传给 entry 的参数；可为 NULL，框架原样转交 */
    uint32_t stack_bytes;     /* 栈字节数，须 >= PF_TASK_MIN_STACK */
} pf_task_desc_t;

/* ── 端口表 ────────────────────────────────────────────────────── */

typedef struct pf_ops {
    /** 结构体布局版本。
     *  入参：无（字段）。
     *  返回：无。
     *  线程与中断：只在初始化期被框架读一次，不涉及并发。
     *  注意：必须等于 PF_OPS_VERSION；不匹配 fw_init() 返回 CM_ERR_VERSION，
     *        框架**不尝试兼容**旧版本——兼容两套语义等于永远养着两套。 */
    uint32_t version;

    /** 能力位图。
     *  入参：无。
     *  返回：位 0–8 表示这块板**确实有**的硬件；位 9–31 必须为 0。
     *  线程与中断：任意任务可调；不许阻塞、不许加锁；boot 之后必须保持稳定。
     *  注意：能力位只说明硬件在，不说明 api/ 里有对应接口；用之前仍要查
     *        api/af_api/cap.h 与 cap_available()。 */
    uint32_t (*caps)(void);

    /** 从平台堆分配。
     *  入参：bytes > 0。bytes == 0 → 返回 NULL（不接受 0 字节分配，避免平台分歧）。
     *  返回：至少 bytes 字节、按 max_align_t 对齐的指针；失败返回 NULL（无错误码）。
     *  线程与中断：可被多个任务并发调用（实现内部自己加锁）；**ISR 里禁止调用**。
     *  注意：这块内存会被长期持有；v1 没有 realloc。超大请求返回 NULL，
     *        不许崩溃、不许静默截断。释放责任在调用方。 */
    void *(*mem_alloc)(size_t bytes);

    /** 归还 mem_alloc 得到的内存。
     *  入参：p 必须是 mem_alloc 返回的指针，或 NULL。
     *  返回：无。
     *  线程与中断：可被多个任务并发调用；**ISR 里禁止调用**。
     *  注意：mem_free(NULL) 必须无操作（合法）。重复释放或释放非本分配器的指针
     *        属于未定义行为，框架不检查；host-fake 在 debug 构建下会主动断言，
     *        让错误在测试里炸而不是在真机上偶发。 */
    void (*mem_free)(void *p);

    /** 单调毫秒计数，从平台启动起算。
     *  入参：无。
     *  返回：单调不减的 uint32_t 毫秒值；约 49.7 天自然回绕（回绕是正常的）。
     *  线程与中断：任意任务与 **ISR** 均可调用（九项里唯一允许 ISR 的）；
     *              不许阻塞、不许加锁。
     *  注意：只能算间隔与超时，不能当墙上时间。比较必须写差值：
     *        (uint32_t)(clock_ms() - start) >= timeout；
     *        写成 now >= start + timeout 在回绕点上是错的。
     *        精度要求：±10% 或 ±1 tick（取大者）。 */
    uint32_t (*clock_ms)(void);

    /** 让当前任务睡至少 ms 毫秒。
     *  入参：ms == 0 → 让出 CPU 一次（让给谁不保证）；其它值无上限。
     *  返回：无。
     *  线程与中断：会阻塞调用者；**ISR 里禁止调用**（ISR 中睡眠是致命错误）。
     *  注意：**必须真的让出 CPU**，不许忙等——实现成忙等会让"伪阻塞"整套机制
     *        失效（fw_delay_block 变空转、看门狗误报）。允许睡得更久，不许更短。 */
    void (*sleep_ms)(uint32_t ms);

    /** 创建一把互斥量（非递归，不可销毁）。
     *  入参：无。
     *  返回：成功 = 不透明句柄；失败 = NULL。
     *  线程与中断：只在 boot 期调用，不走热路径；**ISR 里禁止调用**。
     *  注意：NULL 是**平台违约**而不是"这块板不支持锁"——框架自己的注册表就要锁，
     *        没有锁就没有框架；fw_init() 见到 NULL 返回 CM_ERR_INVALID 拒绝启动。
     *        没有 destroy：锁的生命周期 = 平台生命周期，v1 无动态卸载。 */
    pf_mutex_t *(*mutex_create)(void);

    /** 取锁，最多等 timeout_ms。
     *  入参：m 必须来自 mutex_create()，为 NULL 时返回 -1（不崩溃）；
     *        timeout_ms == 0 表示只试一次、不阻塞。
     *  返回：0 = 拿到；-1 = 没拿到（超时）；其它非 0 值保留给平台错误
     *        （框架一律按"没拿到"处理）。
     *  线程与中断：**ISR 里禁止调用**；阻塞期间必须让出 CPU，不许忙等。
     *  注意：超时是**相对**语义——从调用那一刻起最多等 timeout_ms。
     *        实现成"绝对截止时间"会在超时计算错误时静默出错
     *        （睡 10ms 后 take 5ms 应该失败，而不是等 15ms）。 */
    int (*mutex_take)(pf_mutex_t *m, uint32_t timeout_ms);

    /** 释放锁。
     *  入参：m 必须是当前任务持有的锁；NULL → 无操作。
     *  返回：无。
     *  线程与中断：**ISR 里禁止调用**（ISR 与任务之间用 fw_event_post_isr，不用锁）。
     *  注意：非持有者释放属于未定义行为，平台不检查；框架保证不这么写。
     *        非递归锁：同一任务重复 take 会把自己锁死。 */
    void (*mutex_give)(pf_mutex_t *m);

    /** 创建任务并立即返回。
     *  入参：desc 非 NULL、desc->entry 非 NULL、desc->stack_bytes >= PF_TASK_MIN_STACK；
     *        desc->name 可为 NULL；desc->arg 可为 NULL。
     *  返回：成功 = 不透明句柄（框架只记住，不用它做别的）；失败 = NULL
     *        （栈不够、任务数满、alloc 失败）。
     *  线程与中断：**ISR 里禁止调用**。
     *  注意：新任务**可能已经开始跑**，调用方不能假设"返回时它还没动"。
     *        stack_bytes 太小必须返回 NULL，不许自动抬到最小栈。
     *        栈由平台分配、任务结束时由平台回收；框架不替任务管栈。
     *        调度优先级由平台决定，框架不传、也不允许 App 影响。 */
    pf_task_t *(*task_create)(const pf_task_desc_t *desc);

    /** 结束当前任务。
     *  入参：无。
     *  返回：**不返回**。
     *  线程与中断：只能在任务上下文调用；**ISR 里禁止调用**；
     *              从任务外（例如主任务）调用是未定义的。
     *  注意：这是"框架不认识 RTOS"的实现方式——把各家的
     *        vTaskDelete(NULL) / pthread_exit / return 收敛成一项。
     *        因为 C 编译器不知道函数指针不返回，**框架不得在调用点之后
     *        放清理代码**；清理只能写在 App 的 exit() 回调里。 */
    void (*task_exit_current)(void);
} pf_ops_t;

/** 平台把自己那张表交出来。**由平台实现**，框架在主任务入口里调它一次。
 *
 * @return 指向平台私有常量表的指针。**不得返回 NULL、不得返回栈上/堆上临时表**
 *         （框架只保存这个指针，不做深拷贝）。
 *         线程与中断：只在启动阶段（`app_main()` 内、其余任务还没起）调用一次，
 *                     因此实现里可以用"第一次调用时初始化"的写法，不必加锁。
 *         注意事项：表一旦交出去就**不许再改**——框架会缓存成员指针，改表等于
 *                   让已经缓存的函数指针和 `version` 描述的东西对不上。
 *                   本函数是"平台初始化"与"框架启动"之间唯一的口子，别再加第二个。
 */
const pf_ops_t *pf_platform_ops(void);

/** 打一行已经格式化好的日志。**由平台实现**，框架的 `fw_log()` 最后调它。
 *
 * 为什么不在 `pf_ops_t` 里加一列：加一列就要改布局、就要 +1 `PF_OPS_VERSION`、
 * 就要让所有板子重编签名——而日志不是"框架运转需要的东西"，它是诊断设施。
 * 放在表外，最多是"这块板还没接日志"时链接不过，而不是整张表不兼容。
 *
 * @param line 以 `\0` 结尾的整行文本（不含换行）；**不得为 NULL**。
 *             生命周期：只在本次调用期间有效，实现**不得保存指针**。
 *             线程与中断：任意任务可调；**ISR 里禁止**（`fw_log()` 里已经拦不住，
 *                          要靠调用方自律——串口输出在 ISR 里会阻塞）。
 *         注意事项：实现里不要再调用 `fw_log()`（无限递归）。长度上限由实现决定，
 *                   超出可截断，不得崩溃。
 */
void pf_platform_log(const char *line);

/** 板级启动钩子。**由平台实现**，框架在 `fw_startup()` 里、`fw_init()` 之前调一次。
 *
 * 它存在的**唯一理由**：能力实现（`pf_uart_ops_t` 之类）必须在框架开始启动 App
 * 之前就注册进注册表（`fw_cap_register()`），否则第一个 App 起来时
 * `cap_available()` 会回答"没有"，而硬件明明在。没有这个钩子，板子无处落这行注册。
 *
 * @return 无。失败含义：注册失败的位就是"没有这个能力"，
 *         `fw_init()` 之后 `cap_available()` 会如实回答 false。
 *         线程与中断：只在启动阶段调用一次（`fw_init()` 之前，任务还没起）；
 *                     实现里可以不加锁。
 *         注意事项：**实现必须幂等**——框架保证只调一次，但重复 open 同一路硬件
 *                   应当返回 -1 而不是静默成功；本函数里不要调用任何 `fw_*` 之外的
 *                   会阻塞的等待（App 任务还没起来）。 */
void pf_platform_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_PF_OPS_H */
