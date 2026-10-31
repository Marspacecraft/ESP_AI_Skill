/*
 * tests/common/test_cm_list.c —— 侵入式双向链表的用例。
 *
 * 覆盖：空链、首尾插、首尾摘、中间插入、节点跨链迁移、重复摘除的空操作。
 * 这些都只验单线程语义；并发安全不在本模块承诺范围内（见 cm_list.h 文件头）。
 */

#include "cm_list.h"

#include "tinytest.h"

struct list_item {
    int value;
    cm_list_node_t link;
};

static struct list_item *item_of(cm_list_node_t *node)
{
    if (node == NULL) {
        return NULL;
    }
    return CM_CONTAINER_OF(node, struct list_item, link);
}

TT_CASE(empty_list_has_no_front_or_back)
{
    cm_list_t list;

    cm_list_init(&list);
    TT_ASSERT(cm_list_is_empty(&list));
    TT_ASSERT_INT(0, cm_list_count(&list));
    TT_ASSERT(cm_list_front(&list) == NULL);
    TT_ASSERT(cm_list_back(&list) == NULL);
    TT_ASSERT(cm_list_pop_front(&list) == NULL);
    TT_ASSERT(cm_list_pop_back(&list) == NULL);
}

TT_CASE(push_back_keeps_fifo_order)
{
    cm_list_t list;
    struct list_item a = {1, {NULL, NULL}};
    struct list_item b = {2, {NULL, NULL}};
    struct list_item c = {3, {NULL, NULL}};

    cm_list_init(&list);
    cm_list_push_back(&list, &a.link);
    cm_list_push_back(&list, &b.link);
    cm_list_push_back(&list, &c.link);

    TT_ASSERT_INT(3, cm_list_count(&list));
    TT_ASSERT_INT(1, item_of(cm_list_front(&list))->value);
    TT_ASSERT_INT(3, item_of(cm_list_back(&list))->value);
    TT_ASSERT(cm_list_pop_front(&list) == &a.link);
    TT_ASSERT(cm_list_pop_front(&list) == &b.link);
    TT_ASSERT(cm_list_pop_front(&list) == &c.link);
    TT_ASSERT(cm_list_is_empty(&list));
}

TT_CASE(push_front_reverses_order)
{
    cm_list_t list;
    struct list_item a = {1, {NULL, NULL}};
    struct list_item b = {2, {NULL, NULL}};

    cm_list_init(&list);
    cm_list_push_front(&list, &a.link);
    cm_list_push_front(&list, &b.link);

    TT_ASSERT(cm_list_pop_front(&list) == &b.link);
    TT_ASSERT(cm_list_pop_front(&list) == &a.link);
    TT_ASSERT(cm_list_is_empty(&list));
}

TT_CASE(pop_back_returns_last_and_unlinks)
{
    cm_list_t list;
    struct list_item a = {1, {NULL, NULL}};
    struct list_item b = {2, {NULL, NULL}};

    cm_list_init(&list);
    cm_list_push_back(&list, &a.link);
    cm_list_push_back(&list, &b.link);

    TT_ASSERT(cm_list_pop_back(&list) == &b.link);
    TT_ASSERT(!cm_list_is_linked(&b.link));
    TT_ASSERT(cm_list_is_linked(&a.link));
    TT_ASSERT_INT(1, cm_list_count(&list));
}

TT_CASE(popped_node_can_enter_another_list)
{
    cm_list_t first;
    cm_list_t second;
    struct list_item a = {1, {NULL, NULL}};

    cm_list_init(&first);
    cm_list_init(&second);
    cm_list_push_back(&first, &a.link);
    TT_ASSERT(cm_list_is_linked(&a.link));

    cm_list_push_back(&second, cm_list_pop_front(&first));
    TT_ASSERT(cm_list_is_empty(&first));
    TT_ASSERT_INT(1, cm_list_count(&second));
    TT_ASSERT(cm_list_front(&second) == &a.link);
    TT_ASSERT_INT(1, item_of(cm_list_front(&second))->value);
}

TT_CASE(insert_before_and_after_keep_order)
{
    cm_list_t list;
    struct list_item a = {1, {NULL, NULL}};
    struct list_item b = {2, {NULL, NULL}};
    struct list_item c = {3, {NULL, NULL}};
    struct list_item d = {4, {NULL, NULL}};

    cm_list_init(&list);
    cm_list_push_back(&list, &a.link);
    cm_list_push_back(&list, &c.link);
    cm_list_insert_before(&list, &c.link, &b.link);
    cm_list_insert_after(&list, &c.link, &d.link);

    TT_ASSERT_INT(4, cm_list_count(&list));
    TT_ASSERT(cm_list_pop_front(&list) == &a.link);
    TT_ASSERT(cm_list_pop_front(&list) == &b.link);
    TT_ASSERT(cm_list_pop_front(&list) == &c.link);
    TT_ASSERT(cm_list_pop_front(&list) == &d.link);
}

TT_CASE(remove_middle_node_updates_count)
{
    cm_list_t list;
    struct list_item a = {1, {NULL, NULL}};
    struct list_item b = {2, {NULL, NULL}};
    struct list_item c = {3, {NULL, NULL}};

    cm_list_init(&list);
    cm_list_push_back(&list, &a.link);
    cm_list_push_back(&list, &b.link);
    cm_list_push_back(&list, &c.link);

    cm_list_remove(&list, &b.link);
    TT_ASSERT_INT(2, cm_list_count(&list));
    TT_ASSERT(!cm_list_is_linked(&b.link));
    TT_ASSERT_INT(1, item_of(cm_list_front(&list))->value);
    TT_ASSERT_INT(3, item_of(cm_list_back(&list))->value);
}

TT_CASE(remove_unlinked_node_is_noop)
{
    cm_list_t list;
    struct list_item a = {1, {NULL, NULL}};

    cm_list_init(&list);
    cm_list_node_init(&a.link);
    cm_list_remove(&list, &a.link);
    TT_ASSERT_INT(0, cm_list_count(&list));
    TT_ASSERT(cm_list_is_empty(&list));
}

TT_CASE(node_init_clears_link_state)
{
    struct list_item a = {1, {NULL, NULL}};

    cm_list_node_init(&a.link);
    TT_ASSERT(!cm_list_is_linked(&a.link));
    TT_ASSERT(a.link.prev == NULL);
    TT_ASSERT(a.link.next == NULL);
}

int main(void)
{
    TT_PROGRAM("test_cm_list");

    TT_RUN(empty_list_has_no_front_or_back);
    TT_RUN(push_back_keeps_fifo_order);
    TT_RUN(push_front_reverses_order);
    TT_RUN(pop_back_returns_last_and_unlinks);
    TT_RUN(popped_node_can_enter_another_list);
    TT_RUN(insert_before_and_after_keep_order);
    TT_RUN(remove_middle_node_updates_count);
    TT_RUN(remove_unlinked_node_is_noop);
    TT_RUN(node_init_clears_link_state);

    return tt_summary();
}
