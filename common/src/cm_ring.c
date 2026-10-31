/*
 * cm_ring.c —— SPSC 无锁字节环形缓冲区的实现。
 *
 * 同步协议（写反了不会编译报错，只会偶尔丢数据）：
 *   生产者：读 tail（acquire）→ 算可用空间 → memcpy 数据 → 用 store(release)
 *           发布新 head。release 保证"数据写完了"先于"游标前进了"被消费者看到。
 *   消费者：读 head（acquire）→ 算可读字节 → memcpy 数据 → 用 store(release)
 *           发布新 tail。acquire 保证"看到游标前进"时数据一定已经写完。
 * 两个方向各只有一个写者，所以游标本身不需要 CAS，普通原子读写就够。
 */

#include "cm_ring.h"

#include <string.h>

/* 把 src 的 len 字节写进环缓冲区，从 offset 开始，跨过末尾时折回开头。
 * 调用方保证 len <= size 且 offset < size。 */
static void cm_ring_copy_in(uint8_t *buf, uint32_t size, uint32_t offset,
                            const uint8_t *src, uint32_t len)
{
    uint32_t first = size - offset;

    if (first > len) {
        first = len;
    }
    memcpy(buf + offset, src, first);
    if (len > first) {
        memcpy(buf, src + first, len - first);
    }
}

/* 从环缓冲区复制 len 字节到 dst，从 offset 开始，跨过末尾时折回开头。
 * 调用方保证 len <= size 且 offset < size。 */
static void cm_ring_copy_out(uint8_t *dst, const uint8_t *buf, uint32_t size,
                             uint32_t offset, uint32_t len)
{
    uint32_t first = size - offset;

    if (first > len) {
        first = len;
    }
    memcpy(dst, buf + offset, first);
    if (len > first) {
        memcpy(dst + first, buf, len - first);
    }
}

bool cm_ring_init(cm_ring_t *ring, uint8_t *buf, uint32_t size)
{
    if (ring == NULL) {
        return false;
    }
    /* 容量必须是 2 的幂：位与取模的前提，也是"游标差值自动回绕正确"的前提 */
    if (buf == NULL || size == 0 || (size & (size - 1)) != 0) {
        ring->buf = NULL;
        ring->size = 0;
        ring->mask = 0;
        cm_atomic_u32_store(&ring->head, 0, CM_MEM_ORDER_RELAXED);
        cm_atomic_u32_store(&ring->tail, 0, CM_MEM_ORDER_RELAXED);
        return false;
    }
    ring->buf = buf;
    ring->size = size;
    ring->mask = size - 1;
    /* 先把游标归零再让调用方发布这个对象；此时还没有并发访问者 */
    cm_atomic_u32_store(&ring->head, 0, CM_MEM_ORDER_RELAXED);
    cm_atomic_u32_store(&ring->tail, 0, CM_MEM_ORDER_RELAXED);
    return true;
}

uint32_t cm_ring_capacity(const cm_ring_t *ring)
{
    if (ring == NULL) {
        return 0;
    }
    return ring->size;
}

uint32_t cm_ring_used(const cm_ring_t *ring)
{
    uint32_t head;
    uint32_t tail;
    uint32_t used;

    if (ring == NULL || ring->size == 0) {
        return 0;
    }
    /* 诊断用的近似快照：两次加载之间两端都可能前进，所以结果夹到 [0, size]，
     * 保证不会返回一个比容量还大的数。 */
    head = cm_atomic_u32_load(&ring->head, CM_MEM_ORDER_RELAXED);
    tail = cm_atomic_u32_load(&ring->tail, CM_MEM_ORDER_RELAXED);
    used = head - tail;
    if (used > ring->size) {
        used = ring->size;
    }
    return used;
}

uint32_t cm_ring_free(const cm_ring_t *ring)
{
    if (ring == NULL || ring->size == 0) {
        return 0;
    }
    return ring->size - cm_ring_used(ring);
}

uint32_t cm_ring_write(cm_ring_t *ring, const void *src, uint32_t len)
{
    uint32_t head;
    uint32_t tail;
    uint32_t space;
    uint32_t n;

    if (ring == NULL || src == NULL || len == 0 || ring->size == 0) {
        return 0;
    }
    /* head 只有生产者写，本函数就是唯一生产者，所以读自己用 RELAXED；
     * tail 是对端写的，用 ACQUIRE 才能看到"消费者已释放的空间"对应的数据序。 */
    head = cm_atomic_u32_load(&ring->head, CM_MEM_ORDER_RELAXED);
    tail = cm_atomic_u32_load(&ring->tail, CM_MEM_ORDER_ACQUIRE);
    space = ring->size - (head - tail);
    n = (len < space) ? len : space;
    if (n == 0) {
        return 0;
    }
    cm_ring_copy_in(ring->buf, ring->size, head & ring->mask, (const uint8_t *)src, n);
    /* RELEASE：上面的 memcpy 必须先于新 head 对消费者可见 */
    cm_atomic_u32_store(&ring->head, head + n, CM_MEM_ORDER_RELEASE);
    return n;
}

uint32_t cm_ring_read(cm_ring_t *ring, void *dst, uint32_t len)
{
    uint32_t head;
    uint32_t tail;
    uint32_t avail;
    uint32_t n;

    if (ring == NULL || dst == NULL || len == 0 || ring->size == 0) {
        return 0;
    }
    /* head 是对端写的，用 ACQUIRE 才能保证"看到可读字节"时数据已经写好；
     * tail 只有消费者写，读自己用 RELAXED。 */
    head = cm_atomic_u32_load(&ring->head, CM_MEM_ORDER_ACQUIRE);
    tail = cm_atomic_u32_load(&ring->tail, CM_MEM_ORDER_RELAXED);
    avail = head - tail;
    n = (len < avail) ? len : avail;
    if (n == 0) {
        return 0;
    }
    cm_ring_copy_out((uint8_t *)dst, ring->buf, ring->size, tail & ring->mask, n);
    cm_atomic_u32_store(&ring->tail, tail + n, CM_MEM_ORDER_RELEASE);
    return n;
}

uint32_t cm_ring_peek(const cm_ring_t *ring, void *dst, uint32_t len)
{
    uint32_t head;
    uint32_t tail;
    uint32_t avail;
    uint32_t n;

    if (ring == NULL || dst == NULL || len == 0 || ring->size == 0) {
        return 0;
    }
    head = cm_atomic_u32_load(&ring->head, CM_MEM_ORDER_ACQUIRE);
    tail = cm_atomic_u32_load(&ring->tail, CM_MEM_ORDER_RELAXED);
    avail = head - tail;
    n = (len < avail) ? len : avail;
    if (n == 0) {
        return 0;
    }
    cm_ring_copy_out((uint8_t *)dst, ring->buf, ring->size, tail & ring->mask, n);
    return n;
}

uint32_t cm_ring_discard(cm_ring_t *ring, uint32_t len)
{
    uint32_t head;
    uint32_t tail;
    uint32_t avail;
    uint32_t n;

    if (ring == NULL || len == 0 || ring->size == 0) {
        return 0;
    }
    head = cm_atomic_u32_load(&ring->head, CM_MEM_ORDER_ACQUIRE);
    tail = cm_atomic_u32_load(&ring->tail, CM_MEM_ORDER_RELAXED);
    avail = head - tail;
    n = (len < avail) ? len : avail;
    if (n == 0) {
        return 0;
    }
    cm_atomic_u32_store(&ring->tail, tail + n, CM_MEM_ORDER_RELEASE);
    return n;
}

void cm_ring_reset(cm_ring_t *ring)
{
    if (ring == NULL || ring->size == 0) {
        return;
    }
    /* 约定调用方已停掉两端；这里只做普通原子写，不承担"与并发读写同步"的责任 */
    cm_atomic_u32_store(&ring->head, 0, CM_MEM_ORDER_RELAXED);
    cm_atomic_u32_store(&ring->tail, 0, CM_MEM_ORDER_RELAXED);
}
