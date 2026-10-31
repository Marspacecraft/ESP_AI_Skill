#ifndef COMMON_INCLUDE_CM_ATOMIC_H
#define COMMON_INCLUDE_CM_ATOMIC_H

/*
 * cm_atomic.h —— 原子操作的最小集合（无锁代码的唯一地基）。
 *
 * 为什么不用 <stdatomic.h>：
 *   1. 部分 ESP32 工具链的 C 库对该头支持不完整，换 SDK 版本才炸，排查成本极高；
 *   2. 原子性的可见性靠类型说话最省心——cm_atomic_u32_t 一眼能看出"这不是普通
 *      变量"，而 uint32_t 光看类型分不出来；
 *   3. GCC/Clang 的 __atomic_* 内建的语义在 xtensa / riscv / x86 上一致，
 *      可以直接按 C11 内存序写，不需要为某个架构改代码。
 *
 * 用法总则：
 *   - 原子对象的读改写只能走本头的 cm_atomic_* 函数；直接写 x.v 不是原子操作，
 *     而且会让编译器把相邻的内存访问重排到它前面；
 *   - 两个线程共同访问同一个对象时，必须双方都用原子 API，单方面用是错的；
 *   - 本头不分配内存、不阻塞、可在任务与中断上下文调用。
 */

#include "cm_port.h"
#include "cm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 内存序。
 *
 * 取值刻意与 GCC/Clang 的 __ATOMIC_* 常量一一对应（RELAXED=0 … SEQ_CST=5）：
 * 这样传给 __atomic_* 内建时不需要运行期查表，也不需要 switch 转换，枚举本身
 * 就是内建的参数取值。改这里的数值等于改内建参数，**不要动**。
 *
 * 选序速查（写错不会编译报错，只会偶尔丢数据）：
 *   - 单纯计数、无跨线程数据依赖            → RELAXED
 *   - 消费者先读"就绪标志"再去读数据         → 标志用 ACQUIRE
 *   - 生产者先写数据再置"就绪标志"           → 标志用 RELEASE
 *   - 不确定、或需要全局唯一全序             → SEQ_CST（最慢，但默认安全）
 */
typedef enum {
    CM_MEM_ORDER_RELAXED = __ATOMIC_RELAXED, /* 只保证原子性，不做同步 */
    CM_MEM_ORDER_ACQUIRE = __ATOMIC_ACQUIRE, /* 之后的读写不能移到本操作之前 */
    CM_MEM_ORDER_RELEASE = __ATOMIC_RELEASE, /* 之前的读写不能移到本操作之后 */
    CM_MEM_ORDER_ACQ_REL = __ATOMIC_ACQ_REL, /* 兼具 ACQUIRE 与 RELEASE */
    CM_MEM_ORDER_SEQ_CST = __ATOMIC_SEQ_CST  /* 全局唯一全序，最慢 */
} cm_mem_order_e;

/* 32 位无符号原子量。字段 v 只能通过下面 cm_atomic_u32_* 访问。
 * 线程：多任务共享时必须走原子 API；初始化用 store(RELAXED)，发布前完成。 */
typedef struct {
    uint32_t v; /* 当前值；直接读写它不是原子操作 */
} cm_atomic_u32_t;

/* 32 位有符号原子量。规则同上；差值比较请先转 cm_i32 再看正负。 */
typedef struct {
    int32_t v; /* 当前值；直接读写它不是原子操作 */
} cm_atomic_i32_t;

/* 指针原子量。规则同上；指针宽度由目标架构决定（32 位板上是 4 字节）。 */
typedef struct {
    void *v; /* 当前指针；直接读写它不是原子操作 */
} cm_atomic_ptr_t;

/* CAS 失败时使用的内存序。
 *
 * 为什么需要它：C11/GCC 要求 failure 序不能强于 success 序，
 * ACQ_REL 与 RELEASE 直接当 failure 序是无效组合。这里按标准语义降一档：
 * ACQ_REL → ACQUIRE、RELEASE → RELAXED，其余原样。
 *
 * 入参：order 调用方给 CAS 的 success 内存序
 * 返回：可用于 failure 的内存序
 * 线程：纯函数，任务与中断上下文均可调用
 * 注意：仅供本头内部与需要手工写 CAS 的调用方使用；允许内联（3 条分支，
 *       编译器会在常量参数下折叠掉）。
 */
CM_INLINE int cm_atomic_order_on_fail(cm_mem_order_e order)
{
    if (order == CM_MEM_ORDER_ACQ_REL) {
        return __ATOMIC_ACQUIRE;
    }
    if (order == CM_MEM_ORDER_RELEASE) {
        return __ATOMIC_RELAXED;
    }
    return (int)order;
}

/* ---------------------------------------------------------------- u32 ---- */

/* 原子读 32 位。
 * 入参：a 目标原子量，不能为 NULL（本 API 不做检查）
 *       order 只允许 RELAXED / ACQUIRE / SEQ_CST
 * 返回：读取到的值
 * 线程：任务与中断上下文均可调用
 * 注意：order 为 RELEASE 时内建里等价于 RELAXED，是本头不做拦截的误用 */
CM_INLINE uint32_t cm_atomic_u32_load(const cm_atomic_u32_t *a, cm_mem_order_e order)
{
    return __atomic_load_n(&a->v, (int)order);
}

/* 原子写 32 位。
 * 入参：a 目标原子量，不能为 NULL；value 要写入的值
 *       order 只允许 RELAXED / RELEASE / SEQ_CST
 * 返回：无
 * 线程：任务与中断上下文均可调用
 * 注意：发布共享数据时用 RELEASE，保证之前写的数据先于本标志可见 */
CM_INLINE void cm_atomic_u32_store(cm_atomic_u32_t *a, uint32_t value, cm_mem_order_e order)
{
    __atomic_store_n(&a->v, value, (int)order);
}

/* 原子交换 32 位。
 * 入参：a 目标原子量；value 新值；order 内存序（任意）
 * 返回：交换前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：无条件写，不比较旧值；需要"仅当是旧值时才写"用 cas */
CM_INLINE uint32_t cm_atomic_u32_exchange(cm_atomic_u32_t *a, uint32_t value,
                                          cm_mem_order_e order)
{
    return __atomic_exchange_n(&a->v, value, (int)order);
}

/* 原子加并返回旧值。
 * 入参：a 目标原子量；delta 增量（按无符号回绕，0xFFFFFFFF + 1 == 0）
 *       order 内存序（任意）
 * 返回：加之前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：回绕是定义好的行为，不要自己判溢出 */
CM_INLINE uint32_t cm_atomic_u32_fetch_add(cm_atomic_u32_t *a, uint32_t delta,
                                           cm_mem_order_e order)
{
    return __atomic_fetch_add(&a->v, delta, (int)order);
}

/* 原子减并返回旧值。
 * 入参：a 目标原子量；delta 减量（按无符号回绕）
 *       order 内存序（任意）
 * 返回：减之前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：同上，回绕是定义好的行为 */
CM_INLINE uint32_t cm_atomic_u32_fetch_sub(cm_atomic_u32_t *a, uint32_t delta,
                                           cm_mem_order_e order)
{
    return __atomic_fetch_sub(&a->v, delta, (int)order);
}

/* 原子按位或并返回旧值。用于能力位、状态标志的置位。
 * 入参：a 目标原子量；bits 要置 1 的位；order 内存序（任意）
 * 返回：或之前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：置位不会清掉别的位；想原子地"读-改-写"复合条件用 cas */
CM_INLINE uint32_t cm_atomic_u32_fetch_or(cm_atomic_u32_t *a, uint32_t bits,
                                          cm_mem_order_e order)
{
    return __atomic_fetch_or(&a->v, bits, (int)order);
}

/* 原子按位与并返回旧值。用于清位。
 * 入参：a 目标原子量；mask 掩码（掩码里为 1 的位保持，为 0 的位清零）
 *       order 内存序（任意）
 * 返回：与之前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：mask = 0 会把整个值清零，调用点是清位时容易写反的地方 */
CM_INLINE uint32_t cm_atomic_u32_fetch_and(cm_atomic_u32_t *a, uint32_t mask,
                                           cm_mem_order_e order)
{
    return __atomic_fetch_and(&a->v, mask, (int)order);
}

/* 比较并交换 32 位（强一致，不会伪失败）。
 * 入参：a 目标原子量；expected 期望值指针（**失败时被改写为当前实际值**）
 *       desired 相等时要写入的新值；order success 的内存序（任意）
 * 返回：true 表示写入成功（此时 *expected 不变）；false 表示失败
 * 线程：任务与中断上下文均可调用
 * 注意：expected 必须指向可写内存且不能为 NULL；循环重试时每轮都要重新判断
 *       返回值，不要用旧的 *expected 再赌一次 */
CM_INLINE bool cm_atomic_u32_cas(cm_atomic_u32_t *a, uint32_t *expected, uint32_t desired,
                                 cm_mem_order_e order)
{
    return __atomic_compare_exchange_n(&a->v, expected, desired, false, (int)order,
                                       cm_atomic_order_on_fail(order));
}

/* ---------------------------------------------------------------- i32 ---- */

/* 原子读 32 位有符号数。
 * 入参：a 目标原子量；order 只允许 RELAXED / ACQUIRE / SEQ_CST
 * 返回：读取到的值
 * 线程：任务与中断上下文均可调用
 * 注意：时间差比较请自行转成 cm_i32 再做 < 0 判断 */
CM_INLINE int32_t cm_atomic_i32_load(const cm_atomic_i32_t *a, cm_mem_order_e order)
{
    return __atomic_load_n(&a->v, (int)order);
}

/* 原子写 32 位有符号数。
 * 入参：a 目标原子量；value 要写入的值；order 只允许 RELAXED / RELEASE / SEQ_CST
 * 返回：无
 * 线程：任务与中断上下文均可调用
 * 注意：与 u32 版本相同，发布数据用 RELEASE */
CM_INLINE void cm_atomic_i32_store(cm_atomic_i32_t *a, int32_t value, cm_mem_order_e order)
{
    __atomic_store_n(&a->v, value, (int)order);
}

/* 原子交换 32 位有符号数。
 * 入参：a 目标原子量；value 新值；order 内存序（任意）
 * 返回：交换前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：无条件写，与 u32 版本相同 */
CM_INLINE int32_t cm_atomic_i32_exchange(cm_atomic_i32_t *a, int32_t value,
                                         cm_mem_order_e order)
{
    return __atomic_exchange_n(&a->v, value, (int)order);
}

/* 原子加并返回旧值（有符号）。
 * 入参：a 目标原子量；delta 增量；order 内存序（任意）
 * 返回：加之前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：有符号溢出在 C 里是未定义行为，调用方要保证不会溢出 */
CM_INLINE int32_t cm_atomic_i32_fetch_add(cm_atomic_i32_t *a, int32_t delta,
                                          cm_mem_order_e order)
{
    return __atomic_fetch_add(&a->v, delta, (int)order);
}

/* 原子减并返回旧值（有符号）。
 * 入参：a 目标原子量；delta 减量；order 内存序（任意）
 * 返回：减之前的旧值
 * 线程：任务与中断上下文均可调用
 * 注意：同上，调用方保证不溢出 */
CM_INLINE int32_t cm_atomic_i32_fetch_sub(cm_atomic_i32_t *a, int32_t delta,
                                          cm_mem_order_e order)
{
    return __atomic_fetch_sub(&a->v, delta, (int)order);
}

/* 比较并交换 32 位有符号数。
 * 入参：a 目标原子量；expected 期望值指针（失败时改写为当前实际值）
 *       desired 相等时要写入的新值；order success 的内存序（任意）
 * 返回：true 成功 / false 失败
 * 线程：任务与中断上下文均可调用
 * 注意：expected 不能为 NULL；与 u32 版本语义完全一致 */
CM_INLINE bool cm_atomic_i32_cas(cm_atomic_i32_t *a, int32_t *expected, int32_t desired,
                                 cm_mem_order_e order)
{
    return __atomic_compare_exchange_n(&a->v, expected, desired, false, (int)order,
                                       cm_atomic_order_on_fail(order));
}

/* ---------------------------------------------------------------- ptr ---- */

/* 原子读指针。
 * 入参：a 目标原子量；order 只允许 RELAXED / ACQUIRE / SEQ_CST
 * 返回：读取到的指针（可能为 NULL）
 * 线程：任务与中断上下文均可调用
 * 注意：只保证指针本身原子；指针指向对象的生命周期要另外保证 */
CM_INLINE void *cm_atomic_ptr_load(const cm_atomic_ptr_t *a, cm_mem_order_e order)
{
    return __atomic_load_n(&a->v, (int)order);
}

/* 原子写指针。
 * 入参：a 目标原子量；value 新指针（可为 NULL）；order 只允许 RELAXED / RELEASE / SEQ_CST
 * 返回：无
 * 线程：任务与中断上下文均可调用
 * 注意：发布"指针已就绪"给另一个任务时用 RELEASE */
CM_INLINE void cm_atomic_ptr_store(cm_atomic_ptr_t *a, void *value, cm_mem_order_e order)
{
    __atomic_store_n(&a->v, value, (int)order);
}

/* 原子交换指针。
 * 入参：a 目标原子量；value 新指针；order 内存序（任意）
 * 返回：交换前的旧指针
 * 线程：任务与中断上下文均可调用
 * 注意：无条件写；旧指针的释放责任归拿到它的那一方 */
CM_INLINE void *cm_atomic_ptr_exchange(cm_atomic_ptr_t *a, void *value, cm_mem_order_e order)
{
    return __atomic_exchange_n(&a->v, value, (int)order);
}

/* 比较并交换指针。
 * 入参：a 目标原子量；expected 期望指针的指针（失败时改写为当前实际指针）
 *       desired 相等时要写入的新指针；order success 的内存序（任意）
 * 返回：true 成功 / false 失败
 * 线程：任务与中断上下文均可调用
 * 注意：expected 不能为 NULL；指针相等比较的是地址，不是指向的内容 */
CM_INLINE bool cm_atomic_ptr_cas(cm_atomic_ptr_t *a, void **expected, void *desired,
                                 cm_mem_order_e order)
{
    return __atomic_compare_exchange_n(&a->v, expected, desired, false, (int)order,
                                       cm_atomic_order_on_fail(order));
}

#ifdef __cplusplus
}
#endif

#endif /* COMMON_INCLUDE_CM_ATOMIC_H */
