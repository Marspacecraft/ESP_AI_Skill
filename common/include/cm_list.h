#ifndef COMMON_INCLUDE_CM_LIST_H
#define COMMON_INCLUDE_CM_LIST_H

/*
 * cm_list.h —— 侵入式双向链表。
 *
 * 为什么是侵入式（节点嵌进业务结构体，而不是链表自己分配节点）：
 *   - common/ 不分配内存（docs/coding-style.md §9），链表不能自己 new；
 *   - 一个对象同时挂进多条队列时，每条队列一个 cm_list_node_t 即可，
 *     不需要"节点里再放指针指向元素"的二层结构，也不会多一次间接寻址。
 *
 * 用法：业务结构体里放一个 cm_list_node_t 成员，挂链时传它的地址，
 * 取回时用 CM_CONTAINER_OF(node, 业务类型, 成员名)。
 *
 * 结构：环状双向链表，链表自己持有一个哨兵节点，所以首尾插入/删除都是 O(1)，
 * 也不需要"尾指针"这种东西。
 *
 * 并发：**本模块不加锁、不是线程安全的**。多任务共享同一条链表时由调用方
 * 自己保证互斥（例如只在单一任务里操作，或由调用方持锁）。
 */

#include "cm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 链表节点。嵌入业务结构体，不要单独分配。
 * 未挂链的节点 prev/next 均为 NULL（由 cm_list_node_init 置位）。
 * 线程：本身无状态；同一节点被两个任务同时挂/摘属于调用方错误。 */
typedef struct cm_list_node {
    struct cm_list_node *prev; /* 前驱；未挂链时为 NULL */
    struct cm_list_node *next; /* 后继；未挂链时为 NULL */
} cm_list_node_t;

/* 链表本体。哨兵 head 的 next 指向首元素、prev 指向尾元素；
 * 空链时两者都指回 &head 自身。线程：非线程安全，见文件头。 */
typedef struct {
    cm_list_node_t head; /* 哨兵节点，不属于任何业务对象，不要向它取 CONTAINER_OF */
    uint32_t count;      /* 当前元素个数；由本模块维护 */
} cm_list_t;

/* 初始化链表为空。
 * 入参：list 链表，不能为 NULL
 * 返回：无
 * 线程：任务上下文；不可与同一链表的其他操作并发
 * 注意：重复初始化一条非空链表会丢失原有元素（节点仍互相指着，但链表的
 *       count 归零）——初始化只用于"这块内存刚拿到"的时刻。 */
void cm_list_init(cm_list_t *list);

/* 把节点置为"未挂链"状态。
 * 入参：node 节点，不能为 NULL
 * 返回：无
 * 线程：任务上下文；不可与该节点上的其他操作并发
 * 注意：push/pop 会覆盖 prev/next，正常使用时不必先调用本函数；
 *       只有需要对一块未初始化内存调用 cm_list_is_linked 时才必须先 init。 */
void cm_list_node_init(cm_list_node_t *node);

/* 判断链表是否为空。
 * 入参：list 链表，不能为 NULL
 * 返回：true 空链 / false 非空
 * 线程：任务上下文；只读，但仍不能与写操作并发（无锁保护）
 * 注意：不要用"count == 0"替代本函数来判断哨兵自环，二者在正常使用下一致。 */
bool cm_list_is_empty(const cm_list_t *list);

/* 取元素个数。
 * 入参：list 链表，不能为 NULL
 * 返回：当前元素个数（0 表示空链）
 * 线程：任务上下文；只读
 * 注意：这是 O(1) 的计数，不是遍历出来的；不要在中断里遍历长链。 */
uint32_t cm_list_count(const cm_list_t *list);

/* 取首元素节点。
 * 入参：list 链表，不能为 NULL
 * 返回：首元素节点指针；空链返回 NULL
 * 线程：任务上下文；只读
 * 注意：拿到的节点仍归链表所有；要取业务对象用 CM_CONTAINER_OF。 */
cm_list_node_t *cm_list_front(const cm_list_t *list);

/* 取尾元素节点。
 * 入参：list 链表，不能为 NULL
 * 返回：尾元素节点指针；空链返回 NULL
 * 线程：任务上下文；只读
 * 注意：同 cm_list_front。 */
cm_list_node_t *cm_list_back(const cm_list_t *list);

/* 把节点挂到链表头部。
 * 入参：list 链表；node 待挂节点（必须处于未挂链状态）
 * 返回：无
 * 线程：任务上下文；不可与同一链表的其他操作并发
 * 注意：node 的内存由调用方持有，链表不复制也不释放；重复挂同一节点会
 *       破坏链表结构（本函数不做检查），因此"先摘再挂"由调用方保证。 */
void cm_list_push_front(cm_list_t *list, cm_list_node_t *node);

/* 把节点挂到链表尾部。
 * 入参：list 链表；node 待挂节点（必须处于未挂链状态）
 * 返回：无
 * 线程：任务上下文；不可与同一链表的其他操作并发
 * 注意：同 cm_list_push_front；尾插是队列（FIFO）语义的常用做法。 */
void cm_list_push_back(cm_list_t *list, cm_list_node_t *node);

/* 摘下首元素。
 * 入参：list 链表，不能为 NULL
 * 返回：被摘下的节点（已置为未挂链）；空链返回 NULL
 * 线程：任务上下文；不可与其他操作并发
 * 注意：节点内存仍归调用方，链表不释放；调用方负责把它放回对象池或
 *       重新挂到别的链表。 */
cm_list_node_t *cm_list_pop_front(cm_list_t *list);

/* 摘下尾元素。
 * 入参：list 链表，不能为 NULL
 * 返回：被摘下的节点（已置为未挂链）；空链返回 NULL
 * 线程：任务上下文；不可与其他操作并发
 * 注意：同 cm_list_pop_front。 */
cm_list_node_t *cm_list_pop_back(cm_list_t *list);

/* 把节点插到 anchor 之前。
 * 入参：list 链表；anchor 已在本链中的节点（不能为 NULL）；
 *       node 待插入节点（必须处于未挂链状态）
 * 返回：无
 * 线程：任务上下文；不可与其他操作并发
 * 注意：anchor 必须是本链表的元素——传错链表会让两个链表的节点互相
 *       指着，结构当场毁掉且当场不会报错。 */
void cm_list_insert_before(cm_list_t *list, cm_list_node_t *anchor, cm_list_node_t *node);

/* 把节点插到 anchor 之后。
 * 入参：list 链表；anchor 已在本链中的节点（不能为 NULL）；
 *       node 待插入节点（必须处于未挂链状态）
 * 返回：无
 * 线程：任务上下文；不可与其他操作并发
 * 注意：同 cm_list_insert_before；按优先级插队的实现通常用它。 */
void cm_list_insert_after(cm_list_t *list, cm_list_node_t *anchor, cm_list_node_t *node);

/* 从链表中摘除指定节点。
 * 入参：list 节点所在的链表；node 要摘的节点
 * 返回：无
 * 线程：任务上下文；不可与其他操作并发
 * 注意：node 未挂链时本函数是安全的空操作（不会把 count 减成负数）；
 *       node 挂在**别的**链表上属于调用方错误，本函数查不出来。 */
void cm_list_remove(cm_list_t *list, cm_list_node_t *node);

/* 判断节点当前是否挂在某条链表上。
 * 入参：node 节点，不能为 NULL，且必须已经过 cm_list_node_init 或挂链操作
 * 返回：true 已挂链 / false 未挂链
 * 线程：任务上下文；只读
 * 注意：只能回答"挂没挂"，不能回答"挂在哪条链表上"；对一块从未初始化过的
 *       栈内存调用本函数，结果是无意义的（prev/next 是随机值）。 */
bool cm_list_is_linked(const cm_list_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_INCLUDE_CM_LIST_H */
