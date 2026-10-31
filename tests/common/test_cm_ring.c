/*
 * tests/common/test_cm_ring.c —— SPSC 无锁字节环的用例。
 *
 * 覆盖：参数校验、读写往返、满环部分写、跨末尾回绕、peek 不消费、discard、
 * reset、容量 1 的边界。
 *
 * 注意验证边界：本文件是**单线程**用例，只能证明"协议在单线程交错下是对的"；
 * 两端真并发时的可见性靠 cm_ring.c 里 release/acquire 的配对保证，本机没有
 * 线程夹具，无法在这里复现（已写进交付的 Unverified）。
 */

#include "cm_ring.h"

#include "tinytest.h"

static uint8_t g_buf[16];
static cm_ring_t g_ring;

TT_CASE(init_rejects_invalid_arguments)
{
    uint8_t raw[4];

    TT_ASSERT(!cm_ring_init(NULL, raw, 4));
    TT_ASSERT(!cm_ring_init(&g_ring, NULL, 4));
    TT_ASSERT(!cm_ring_init(&g_ring, raw, 0));
    TT_ASSERT(!cm_ring_init(&g_ring, raw, 3)); /* 3 不是 2 的幂 */
    /* 失败后必须是"不可用"状态：容量 0，写不进去，也不能拿野指针去 memcpy */
    TT_ASSERT_INT(0, cm_ring_capacity(&g_ring));
    TT_ASSERT_INT(0, cm_ring_write(&g_ring, raw, 1));
    TT_ASSERT_INT(0, cm_ring_read(&g_ring, raw, 1));
}

TT_CASE(init_reports_empty_capacity)
{
    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 4));
    TT_ASSERT_INT(4, cm_ring_capacity(&g_ring));
    TT_ASSERT_INT(0, cm_ring_used(&g_ring));
    TT_ASSERT_INT(4, cm_ring_free(&g_ring));
}

TT_CASE(write_then_read_roundtrip)
{
    static const uint8_t in[4] = {0x10, 0x20, 0x30, 0x40};
    uint8_t out[4] = {0};
    int i;

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 8));
    TT_ASSERT_INT(4, cm_ring_write(&g_ring, in, 4));
    TT_ASSERT_INT(4, cm_ring_used(&g_ring));
    TT_ASSERT_INT(4, cm_ring_free(&g_ring));
    TT_ASSERT_INT(4, cm_ring_read(&g_ring, out, 4));
    for (i = 0; i < 4; i++) {
        TT_ASSERT_INT(in[i], out[i]);
    }
    TT_ASSERT_INT(0, cm_ring_used(&g_ring));
}

TT_CASE(full_ring_reports_partial_write)
{
    static const uint8_t first[3] = {'x', 'x', 'x'};
    static const uint8_t second[3] = {'y', 'y', 'y'};
    uint8_t out[4] = {0};

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 4));
    TT_ASSERT_INT(3, cm_ring_write(&g_ring, first, 3));
    /* 只剩 1 字节空间：允许部分写入，返回实际写入量，不是错误 */
    TT_ASSERT_INT(1, cm_ring_write(&g_ring, second, 3));
    TT_ASSERT_INT(4, cm_ring_used(&g_ring));
    TT_ASSERT_INT(0, cm_ring_write(&g_ring, second, 1)); /* 已满 */
    TT_ASSERT_INT(4, cm_ring_read(&g_ring, out, 4));
    TT_ASSERT_INT('x', out[0]);
    TT_ASSERT_INT('x', out[1]);
    TT_ASSERT_INT('x', out[2]);
    TT_ASSERT_INT('y', out[3]);
}

TT_CASE(write_across_end_wraps_correctly)
{
    static const uint8_t first[5] = {1, 2, 3, 4, 5};
    static const uint8_t second[6] = {6, 7, 8, 9, 10, 11};
    uint8_t out[8] = {0};
    int i;

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 8));
    TT_ASSERT_INT(5, cm_ring_write(&g_ring, first, 5));
    TT_ASSERT_INT(3, cm_ring_read(&g_ring, out, 3)); /* 消费 1,2,3 */
    /* 此时 head=5、tail=3，剩余 2 字节；再写 6 字节会先在偏移 5..7 写 3 字节，
     * 再折回偏移 0..2 写另外 3 字节。 */
    TT_ASSERT_INT(6, cm_ring_write(&g_ring, second, 6));
    TT_ASSERT_INT(8, cm_ring_used(&g_ring));
    TT_ASSERT_INT(8, cm_ring_read(&g_ring, out, 8));
    {
        static const uint8_t expected[8] = {4, 5, 6, 7, 8, 9, 10, 11};
        for (i = 0; i < 8; i++) {
            TT_ASSERT_INT(expected[i], out[i]);
        }
    }
    TT_ASSERT_INT(0, cm_ring_used(&g_ring));
}

TT_CASE(peek_does_not_consume)
{
    static const uint8_t in[3] = {'a', 'b', 'c'};
    uint8_t out[8] = {0};

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 8));
    TT_ASSERT_INT(3, cm_ring_write(&g_ring, in, 3));
    TT_ASSERT_INT(2, cm_ring_peek(&g_ring, out, 2));
    TT_ASSERT_INT('a', out[0]);
    TT_ASSERT_INT('b', out[1]);
    TT_ASSERT_INT(3, cm_ring_used(&g_ring)); /* peek 不改变已用量 */
    TT_ASSERT_INT(3, cm_ring_peek(&g_ring, out, 8)); /* 只复制 3 字节 */
    TT_ASSERT_INT(3, cm_ring_read(&g_ring, out, 3));
    TT_ASSERT_INT('c', out[2]);
    TT_ASSERT_INT(0, cm_ring_used(&g_ring));
}

TT_CASE(discard_consumes_without_copy)
{
    static const uint8_t in[4] = {9, 8, 7, 6};
    uint8_t out[2] = {0};

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 4));
    TT_ASSERT_INT(4, cm_ring_write(&g_ring, in, 4));
    TT_ASSERT_INT(2, cm_ring_discard(&g_ring, 2));
    TT_ASSERT_INT(2, cm_ring_used(&g_ring));
    TT_ASSERT_INT(2, cm_ring_read(&g_ring, out, 2));
    TT_ASSERT_INT(7, out[0]);
    TT_ASSERT_INT(6, out[1]);
}

TT_CASE(reset_empties_ring)
{
    static const uint8_t in[2] = {'p', 'q'};
    uint8_t out[2] = {0};

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 4));
    TT_ASSERT_INT(2, cm_ring_write(&g_ring, in, 2));
    cm_ring_reset(&g_ring);
    TT_ASSERT_INT(0, cm_ring_used(&g_ring));
    TT_ASSERT_INT(4, cm_ring_free(&g_ring));
    TT_ASSERT_INT(2, cm_ring_write(&g_ring, in, 2));
    TT_ASSERT_INT(2, cm_ring_read(&g_ring, out, 2));
    TT_ASSERT_INT('p', out[0]);
}

TT_CASE(capacity_one_holds_one_byte)
{
    static const uint8_t in[2] = {'m', 'n'};
    uint8_t out[1] = {0};

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 1));
    TT_ASSERT_INT(1, cm_ring_capacity(&g_ring));
    TT_ASSERT_INT(1, cm_ring_write(&g_ring, in, 2));
    TT_ASSERT_INT(0, cm_ring_write(&g_ring, in, 1));
    TT_ASSERT_INT(1, cm_ring_read(&g_ring, out, 1));
    TT_ASSERT_INT('m', out[0]);
}

TT_CASE(invalid_arguments_return_zero)
{
    static const uint8_t in[1] = {1};
    uint8_t out[1] = {0};

    TT_ASSERT(cm_ring_init(&g_ring, g_buf, 4));
    TT_ASSERT_INT(0, cm_ring_write(NULL, in, 1));
    TT_ASSERT_INT(0, cm_ring_write(&g_ring, NULL, 1));
    TT_ASSERT_INT(0, cm_ring_write(&g_ring, in, 0));
    TT_ASSERT_INT(0, cm_ring_read(NULL, out, 1));
    TT_ASSERT_INT(0, cm_ring_read(&g_ring, NULL, 1));
    TT_ASSERT_INT(0, cm_ring_peek(&g_ring, NULL, 1));
    TT_ASSERT_INT(0, cm_ring_discard(&g_ring, 0));
    TT_ASSERT_INT(0, cm_ring_capacity(NULL));
    TT_ASSERT_INT(0, cm_ring_used(NULL));
    TT_ASSERT_INT(0, cm_ring_free(NULL));
}

int main(void)
{
    TT_PROGRAM("test_cm_ring");

    TT_RUN(init_rejects_invalid_arguments);
    TT_RUN(init_reports_empty_capacity);
    TT_RUN(write_then_read_roundtrip);
    TT_RUN(full_ring_reports_partial_write);
    TT_RUN(write_across_end_wraps_correctly);
    TT_RUN(peek_does_not_consume);
    TT_RUN(discard_consumes_without_copy);
    TT_RUN(reset_empties_ring);
    TT_RUN(capacity_one_holds_one_byte);
    TT_RUN(invalid_arguments_return_zero);

    return tt_summary();
}
