/*
 * tests/common/test_cm_atomic.c —— 原子操作的用例。
 *
 * 覆盖：load/store 往返、fetch_add/sub 的返回值与回绕、exchange、
 * fetch_or/and 的位语义、CAS 成功/失败（失败时 *expected 被改写）、
 * i32 版本、指针版本、以及 CAS 失败内存序的降档规则。
 *
 * 验证边界：单线程下只能验证"语义与返回值"，验证不了真正的可见性；
 * 后者由 cm_atomic.h 使用 __atomic_* 内建来保证（已写进 Unverified）。
 */

#include "cm_atomic.h"

#include "tinytest.h"

TT_CASE(u32_store_load_roundtrip)
{
    cm_atomic_u32_t counter;

    cm_atomic_u32_store(&counter, 123, CM_MEM_ORDER_RELAXED);
    TT_ASSERT_INT(123, cm_atomic_u32_load(&counter, CM_MEM_ORDER_ACQUIRE));
    cm_atomic_u32_store(&counter, 0, CM_MEM_ORDER_RELEASE);
    TT_ASSERT_INT(0, cm_atomic_u32_load(&counter, CM_MEM_ORDER_SEQ_CST));
}

TT_CASE(u32_fetch_add_returns_previous)
{
    cm_atomic_u32_t counter;

    cm_atomic_u32_store(&counter, 10, CM_MEM_ORDER_RELAXED);
    TT_ASSERT_INT(10, cm_atomic_u32_fetch_add(&counter, 5, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(15, cm_atomic_u32_load(&counter, CM_MEM_ORDER_RELAXED));
}

TT_CASE(u32_fetch_sub_wraps_around)
{
    cm_atomic_u32_t counter;

    cm_atomic_u32_store(&counter, 0, CM_MEM_ORDER_RELAXED);
    TT_ASSERT_INT(0, cm_atomic_u32_fetch_sub(&counter, 1, CM_MEM_ORDER_RELAXED));
    /* 无符号回绕是定义好的行为：0 - 1 == 0xFFFFFFFF */
    TT_ASSERT_INT(UINT32_MAX, cm_atomic_u32_load(&counter, CM_MEM_ORDER_RELAXED));
}

TT_CASE(u32_exchange_returns_old)
{
    cm_atomic_u32_t value;

    cm_atomic_u32_store(&value, 1, CM_MEM_ORDER_RELAXED);
    TT_ASSERT_INT(1, cm_atomic_u32_exchange(&value, 7, CM_MEM_ORDER_ACQ_REL));
    TT_ASSERT_INT(7, cm_atomic_u32_load(&value, CM_MEM_ORDER_RELAXED));
}

TT_CASE(u32_bit_operations)
{
    cm_atomic_u32_t flags;

    cm_atomic_u32_store(&flags, 0x0F0Fu, CM_MEM_ORDER_RELAXED);
    TT_ASSERT_INT(0x0F0F, cm_atomic_u32_fetch_or(&flags, 0xF000u, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(0xFF0F, cm_atomic_u32_load(&flags, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(0xFF0F, cm_atomic_u32_fetch_and(&flags, 0x00FFu, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(0x000F, cm_atomic_u32_load(&flags, CM_MEM_ORDER_RELAXED));
}

TT_CASE(u32_cas_success_keeps_expected)
{
    cm_atomic_u32_t value;
    uint32_t expected = 5;

    cm_atomic_u32_store(&value, 5, CM_MEM_ORDER_RELAXED);
    TT_ASSERT(cm_atomic_u32_cas(&value, &expected, 9, CM_MEM_ORDER_ACQ_REL));
    TT_ASSERT_INT(5, expected); /* 成功时 *expected 不被改写 */
    TT_ASSERT_INT(9, cm_atomic_u32_load(&value, CM_MEM_ORDER_RELAXED));
}

TT_CASE(u32_cas_failure_updates_expected)
{
    cm_atomic_u32_t value;
    uint32_t expected = 3;

    cm_atomic_u32_store(&value, 5, CM_MEM_ORDER_RELAXED);
    TT_ASSERT(!cm_atomic_u32_cas(&value, &expected, 9, CM_MEM_ORDER_ACQ_REL));
    TT_ASSERT_INT(5, expected); /* 失败时 *expected 变成当前实际值 */
    TT_ASSERT_INT(5, cm_atomic_u32_load(&value, CM_MEM_ORDER_RELAXED));
}

TT_CASE(i32_arithmetic_and_cas)
{
    cm_atomic_i32_t value;
    int32_t expected;

    cm_atomic_i32_store(&value, -5, CM_MEM_ORDER_RELAXED);
    TT_ASSERT_INT(-5, cm_atomic_i32_fetch_add(&value, -3, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(-8, cm_atomic_i32_load(&value, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(-8, cm_atomic_i32_fetch_sub(&value, -2, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(-6, cm_atomic_i32_load(&value, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(-6, cm_atomic_i32_exchange(&value, -6, CM_MEM_ORDER_RELAXED));

    expected = -100;
    TT_ASSERT(!cm_atomic_i32_cas(&value, &expected, 0, CM_MEM_ORDER_RELAXED));
    TT_ASSERT_INT(-6, expected);
}

TT_CASE(ptr_store_load_and_exchange)
{
    int first = 1;
    int second = 2;
    cm_atomic_ptr_t slot;

    cm_atomic_ptr_store(&slot, &first, CM_MEM_ORDER_RELAXED);
    TT_ASSERT(cm_atomic_ptr_load(&slot, CM_MEM_ORDER_ACQUIRE) == (void *)&first);
    TT_ASSERT(cm_atomic_ptr_exchange(&slot, &second, CM_MEM_ORDER_ACQ_REL) ==
              (void *)&first);
    TT_ASSERT(cm_atomic_ptr_load(&slot, CM_MEM_ORDER_RELAXED) == (void *)&second);
}

TT_CASE(ptr_cas_failure_updates_expected)
{
    int first = 1;
    int second = 2;
    void *expected = &first;
    cm_atomic_ptr_t slot;

    cm_atomic_ptr_store(&slot, &second, CM_MEM_ORDER_RELAXED);
    TT_ASSERT(!cm_atomic_ptr_cas(&slot, &expected, &first, CM_MEM_ORDER_ACQ_REL));
    TT_ASSERT(expected == (void *)&second);
    TT_ASSERT(cm_atomic_ptr_load(&slot, CM_MEM_ORDER_RELAXED) == (void *)&second);
}

TT_CASE(failure_order_downgrades_strength)
{
    TT_ASSERT_INT(CM_MEM_ORDER_ACQUIRE,
                  cm_atomic_order_on_fail(CM_MEM_ORDER_ACQ_REL));
    TT_ASSERT_INT(CM_MEM_ORDER_RELAXED,
                  cm_atomic_order_on_fail(CM_MEM_ORDER_RELEASE));
    TT_ASSERT_INT(CM_MEM_ORDER_SEQ_CST,
                  cm_atomic_order_on_fail(CM_MEM_ORDER_SEQ_CST));
    TT_ASSERT_INT(CM_MEM_ORDER_ACQUIRE,
                  cm_atomic_order_on_fail(CM_MEM_ORDER_ACQUIRE));
}

int main(void)
{
    TT_PROGRAM("test_cm_atomic");

    TT_RUN(u32_store_load_roundtrip);
    TT_RUN(u32_fetch_add_returns_previous);
    TT_RUN(u32_fetch_sub_wraps_around);
    TT_RUN(u32_exchange_returns_old);
    TT_RUN(u32_bit_operations);
    TT_RUN(u32_cas_success_keeps_expected);
    TT_RUN(u32_cas_failure_updates_expected);
    TT_RUN(i32_arithmetic_and_cas);
    TT_RUN(ptr_store_load_and_exchange);
    TT_RUN(ptr_cas_failure_updates_expected);
    TT_RUN(failure_order_downgrades_strength);

    return tt_summary();
}
