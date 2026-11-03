#ifndef ESP_AI_SKILL_CM_ATOMIC_H
#define ESP_AI_SKILL_CM_ATOMIC_H

/* 原子访问：跨任务、跨中断共享的标量与指针，**只经这里**读写。
 *
 * 为什么有这一份：`volatile` 只挡编译器优化，不提供原子性，也不给线程模型
 * 任何"这件事发生在那件事之前"的承诺——C11 里它仍是数据竞争（UB）。在多核
 * ESP32 上，编译器把一次 `cancel = 1` 重排到 `state = RUNNING` 之后完全合法，
 * 现场表现是"请求了停止但状态还在跑"。这个坑在 2.4 里被 TSan 一次性抓出七处。
 *
 * 为什么不用 `<stdatomic.h>`：老工具链对 `_Atomic` 支持不全；`__atomic_*` 是
 * GCC/Clang 共有的、32 位无需 libatomic 的路径。将来迁移到 `atomic_uint` 只改
 * 这一个文件，不改语义。
 *
 * 为什么是宏而不是函数：一条宏直接把指针交给内建，**宽度与类型由调用方决定**
 * （uint8_t / uint32_t / size_t / 指针都对），不可能出现"32 位机上把 size_t
 * 悄悄截成 32 位"这种只在一边发生的错。
 *
 * 用哪一档序（不是所有地方都该 ACQUIRE/RELEASE）：
 *   - 状态位、取消位：写 RELEASE / 读 ACQUIRE。写的一方"把话说出去"，读的一方
 *     必须看见写之前发生的事（否则读到 STOPPED，却读不到 exit() 里写的计数）。
 *   - 计数器、心跳序号：RELAXED。只要求"读到一个完整的值"，不要求同步别人。
 *   - "恰好一次"（如 `exit_calls`）：EXCHANGE + ACQ_REL，靠返回的旧值判定。
 *   - 一次性发布的指针（如平台表）：写 RELEASE / 读 ACQUIRE。
 */

typedef enum cm_mem_order_e {
    CM_MEM_RELAXED = 0,
    CM_MEM_CONSUME = 1,
    CM_MEM_ACQUIRE = 2,
    CM_MEM_RELEASE = 3,
    CM_MEM_ACQ_REL = 4,
    CM_MEM_SEQ_CST = 5
} cm_mem_order_e;

/* 数值刻意等于 GCC/Clang 的 `__ATOMIC_*`，所以下面是一次直接转发。
 * 若哪天要换实现，**改这里**，调用方一行不用动。 */

/** 原子读：返回读到的那份值。`p` 非 NULL（调用方保证）。 */
#define CM_ATOMIC_LOAD(p, order)          __atomic_load_n((p), (int)(order))

/** 原子写：无返回值。`p` 非 NULL。 */
#define CM_ATOMIC_STORE(p, v, order)      __atomic_store_n((p), (v), (int)(order))

/** 原子交换：返回**旧值**。"恰好一次"这类语义靠它。 */
#define CM_ATOMIC_EXCHANGE(p, v, order)   __atomic_exchange_n((p), (v), (int)(order))

/** 原子加：返回加之后的**新值**（配额判定用它"先加后判再回滚"）。 */
#define CM_ATOMIC_ADD_FETCH(p, v, order)  __atomic_add_fetch((p), (v), (int)(order))

/** 原子减：返回减之后的**新值**。 */
#define CM_ATOMIC_SUB_FETCH(p, v, order)  __atomic_sub_fetch((p), (v), (int)(order))

#endif /* ESP_AI_SKILL_CM_ATOMIC_H */
