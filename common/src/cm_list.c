/*
 * cm_list.c —— 侵入式双向链表的实现。
 *
 * 这里放 .c 而不是写在头文件里当 static inline 的理由：这些函数的函数体
 * 都超过 5 行（docs/coding-style.md §9 只允许 ≤5 行的头内内联），而且链表
 * 操作在所有调用点都是同一份代码，内联只会让固件体积变大。
 *
 * 结构约定：head 是哨兵，head.next 是首元素、head.prev 是尾元素，
 * 空链时两者都指回 &list->head。所有插入/删除都只改指针，不分配、不复制。
 */

#include "cm_list.h"

#include <stddef.h>

void cm_list_init(cm_list_t *list)
{
    list->head.prev = &list->head;
    list->head.next = &list->head;
    list->count = 0;
}

void cm_list_node_init(cm_list_node_t *node)
{
    node->prev = NULL;
    node->next = NULL;
}

bool cm_list_is_empty(const cm_list_t *list)
{
    return list->head.next == &list->head;
}

uint32_t cm_list_count(const cm_list_t *list)
{
    return list->count;
}

cm_list_node_t *cm_list_front(const cm_list_t *list)
{
    if (list->head.next == &list->head) {
        return NULL;
    }
    return list->head.next;
}

cm_list_node_t *cm_list_back(const cm_list_t *list)
{
    if (list->head.prev == &list->head) {
        return NULL;
    }
    return list->head.prev;
}

/* 把 node 插到 prev 与 next 之间。prev/next 都来自同一条链表。 */
static void cm_list_link(cm_list_node_t *prev, cm_list_node_t *node,
                         cm_list_node_t *next)
{
    node->prev = prev;
    node->next = next;
    prev->next = node;
    next->prev = node;
}

/* 把 node 从它当前所处的位置摘下来，并置为未挂链。 */
static void cm_list_unlink(cm_list_node_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
}

void cm_list_push_front(cm_list_t *list, cm_list_node_t *node)
{
    cm_list_link(&list->head, node, list->head.next);
    list->count++;
}

void cm_list_push_back(cm_list_t *list, cm_list_node_t *node)
{
    cm_list_link(list->head.prev, node, &list->head);
    list->count++;
}

cm_list_node_t *cm_list_pop_front(cm_list_t *list)
{
    cm_list_node_t *node = cm_list_front(list);
    if (node == NULL) {
        return NULL;
    }
    cm_list_unlink(node);
    list->count--;
    return node;
}

cm_list_node_t *cm_list_pop_back(cm_list_t *list)
{
    cm_list_node_t *node = cm_list_back(list);
    if (node == NULL) {
        return NULL;
    }
    cm_list_unlink(node);
    list->count--;
    return node;
}

void cm_list_insert_before(cm_list_t *list, cm_list_node_t *anchor, cm_list_node_t *node)
{
    cm_list_link(anchor->prev, node, anchor);
    list->count++;
}

void cm_list_insert_after(cm_list_t *list, cm_list_node_t *anchor, cm_list_node_t *node)
{
    cm_list_link(anchor, node, anchor->next);
    list->count++;
}

void cm_list_remove(cm_list_t *list, cm_list_node_t *node)
{
    if (!cm_list_is_linked(node)) {
        return;
    }
    cm_list_unlink(node);
    list->count--;
}

bool cm_list_is_linked(const cm_list_node_t *node)
{
    return node->next != NULL;
}
