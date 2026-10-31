#ifndef COMMON_INCLUDE_CM_RING_H
#define COMMON_INCLUDE_CM_RING_H

/*
 * cm_ring.h —— 单生产者/单消费者（SPSC）无锁字节环形缓冲区。
 *
 * 它解决什么问题：中断/DMA 在一个上下文里产出字节，任务在另一个上下文里
 * 消费字节，两边都不能等对方。加锁在中断里是错的（关中断时间不可控），
 * 所以在 SPSC 这个约束下用两个原子游标做无锁交接。
 *
 * 约束（违反任何一条都会出现"偶尔丢数据"这种最难查的故障）：
 *   1. **只有一个生产者**：只允许一个上下文调用 write（任务或中断，二选一，
 *      不能两边都写）；
 *   2. **只有一个消费者**：只允许一个上下文调用 read/peek/discard；
 *   3. 容量必须是 2 的幂（用位与代替取模）；
 *   4. init/reset 只允许在两端都停下来时调用。
 *
 * 为什么用"单调递增游标 + 掩码"而不是"读写下标 + 空/满标志"：
 *   后者在环满时会出现"空"和"满"状态相同、只能靠浪费一个字节来区分的问题；
 *   前者用 head - tail 自然得到已用字节数，容量可以 100% 用满，游标回绕也
 *   由无符号减法自动处理（容量是 2 的幂，回绕前后的差值仍然正确）。
 *
 * 缓冲区由调用方提供：本模块不分配内存（common/ 的硬约束）。
 */

#include "cm_atomic.h"
#include "cm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPSC 字节环。
 * 字段 head/tail 是原子游标，业务代码不要直接读写；buf/size/mask 只在
 * init 时写一次，之后只读。
 * 线程：生产者只写 head、消费者只写 tail；两端可并发。 */
typedef struct {
    uint8_t *buf;          /* 数据缓冲区，由调用方提供并保证生命周期 */
    uint32_t size;         /* 容量，2 的幂；0 表示未初始化 */
    uint32_t mask;         /* size - 1，用于把游标映射成下标 */
    cm_atomic_u32_t head;  /* 生产者写：累计写入字节数（只增，允许回绕） */
    cm_atomic_u32_t tail;  /* 消费者写：累计读出字节数（只增，允许回绕） */
} cm_ring_t;

/* 初始化环形缓冲区。
 * 入参：ring 目标对象，不能为 NULL
 *       buf  数据缓冲区，不能为 NULL，由调用方持有，生命周期必须覆盖整个使用期
 *       size 容量，必须是 2 的幂（1、2、4、8 …），0 会被拒绝
 * 返回：true 初始化成功；false 参数非法（ring 为 NULL 无法写入，返回 false
 *       且不做任何事；其余失败会把 ring 置为全零的"不可用"状态）
 * 线程：任务上下文；不可与任何读写并发（此时环还没有使用者）
 * 注意：buf 里的旧内容不会被清零，第一次 read 之前必须先 write；
 *       容量 1 合法但毫无意义（每次最多读写 1 字节）。 */
bool cm_ring_init(cm_ring_t *ring, uint8_t *buf, uint32_t size);

/* 取容量。
 * 入参：ring 环，不能为 NULL
 * 返回：初始化时给定的容量；未初始化返回 0
 * 线程：任务与中断上下文均可调用
 * 注意：容量是常量，读写不会改变它。 */
uint32_t cm_ring_capacity(const cm_ring_t *ring);

/* 取当前已用字节数（近似值）。
 * 入参：ring 环，不能为 NULL
 * 返回：已写入但尚未读出的字节数；未初始化返回 0
 * 线程：任务与中断上下文均可调用
 * 注意：两端并发时读到的是"某一瞬间"的快照，下一次调用可能已经变了；
 *       不要用"used == 0 说明消费者读完了"来同步业务状态，那不是同步原语。 */
uint32_t cm_ring_used(const cm_ring_t *ring);

/* 取当前剩余可写字节数（近似值）。
 * 入参：ring 环，不能为 NULL
 * 返回：还能写入的字节数；未初始化返回 0
 * 线程：任务与中断上下文均可调用
 * 注意：与 cm_ring_used 相同，是快照；真正的"能写多少"由 cm_ring_write
 *       的返回值决定（并发下快照可能已经过期）。 */
uint32_t cm_ring_free(const cm_ring_t *ring);

/* 写入字节。
 * 入参：ring 环；src 源缓冲区（不能为 NULL）；len 期望写入的字节数
 * 返回：实际写入的字节数，范围 0..len；环满或参数非法时返回 0
 * 线程：**只允许唯一生产者上下文调用**（任务或中断二选一）
 * 注意：允许部分写入（剩余空间不足时只写能放下的部分），调用方必须用
 *       返回值判断是否需要后续重试；空间不足**不算错误**，不要当异常处理。 */
uint32_t cm_ring_write(cm_ring_t *ring, const void *src, uint32_t len);

/* 读出字节。
 * 入参：ring 环；dst 目标缓冲区（不能为 NULL）；len 期望读出的字节数
 * 返回：实际读出的字节数，范围 0..len；环空或参数非法时返回 0
 * 线程：**只允许唯一消费者上下文调用**
 * 注意：读出的字节会从环中移除；只想知道内容不想消费用 cm_ring_peek；
 *       允许部分读取。 */
uint32_t cm_ring_read(cm_ring_t *ring, void *dst, uint32_t len);

/* 窥视字节：复制但不消费。
 * 入参：ring 环；dst 目标缓冲区（不能为 NULL）；len 期望复制的字节数
 * 返回：实际复制的字节数，范围 0..len；环空或参数非法时返回 0
 * 线程：**只允许唯一消费者上下文调用**（与 read 同侧，不能与 read 并发）
 * 注意：peek 与随后的 read 之间生产者可能追加数据，但本侧 tail 尚未移动，
 *       所以"先 peek 再按 peek 到的长度 read"拿到的仍是同一段最旧数据；
 *       读到的是 FIFO 顺序，不会跳过未读字节。 */
uint32_t cm_ring_peek(const cm_ring_t *ring, void *dst, uint32_t len);

/* 丢弃字节：消费但不复制。
 * 入参：ring 环；len 期望丢弃的字节数
 * 返回：实际丢弃的字节数，范围 0..len；环空或参数非法时返回 0
 * 线程：**只允许唯一消费者上下文调用**
 * 注意：用于"这一帧校验失败，整帧扔掉"的场景；丢弃是不可恢复的。 */
uint32_t cm_ring_discard(cm_ring_t *ring, uint32_t len);

/* 清空环。
 * 入参：ring 环，不能为 NULL
 * 返回：无
 * 线程：任务上下文；**必须保证生产者与消费者都已停止**，否则会与并发读写
 *       竞争，游标可能倒退
 * 注意：只重置游标，不清 buf 内容；初始化失败的环调用它是安全的空操作。 */
void cm_ring_reset(cm_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_INCLUDE_CM_RING_H */
