/* platform/host-fake/pf_host_fake.c —— 参考实现（步骤 1.3）。
 *
 * 这不是"某块板"，而是**能在这台开发机上跑起来的平台**：用 glibc/macOS 的
 * pthread + malloc + 单调时钟。它的用途只有一个——让框架与 App 的逻辑能在 host
 * 上被测试；它不模拟真机的时序、带宽、中断与内存紧张。
 *
 * 全仓**只有这里**允许出现 malloc/free 的直接调用（coding-style §9）。
 * 其他任何地方出现，都说明有人绕过了 mem_alloc。
 *
 * 已知与真机的差异（都写在这里，免得有人拿 host 结果当硬件结论）：
 *   - 任务就是 pthread，没有优先级分带；抢占取决于宿主机调度。
 *   - `mutex_take` 的超时用 1ms 粒度轮询实现（macOS 没有 pthread_mutex_timedlock）。
 *     语义是对的（相对超时、期间让出 CPU），精度是假的。
 *   - 任务结构体不回收（进程内测试量很小）；真机实现由平台自己管生命周期。
 *   - caps() 返回 0：这块"板"什么硬件都没有，服务要由测试自己注册假实现。
 */
/* `pthread_setname_np` 是 **GNU 扩展**：glibc 只在 `_GNU_SOURCE` 下声明它，
 * 而 `-std=c11` 会定义 `__STRICT_ANSI__`、让 <features.h> 默认不暴露 GNU 符号，
 * 于是 Linux 上编译报 implicit declaration，`-Werror` 直接停下
 * （CI 原文：ubuntu 作业里 `pf_host_fake.c` 隐式声明 `pthread_setname_np`）。
 *
 * 三条约束：
 *   1. 必须写在**任何系统头之前**——<features.h> 一旦被处理过，再定义就不生效。
 *   2. 只对 Linux 生效；macOS 那边同一个函数由 `_DARWIN_C_SOURCE`（build.sh 里给的）负责。
 *   3. 不放进 build.sh 的 `DEFS`：那是全局开关，会把 GNU 扩展塞给所有文件；
 *      全仓只有这一个文件需要给线程起名。
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "platform/host-fake/pf_host_fake.h"

#include <errno.h>
#include <limits.h> /* PTHREAD_STACK_MIN */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/port/pf_cap.h"
#include "platform/port/pf_uart.h"

/* ── 内存 ──────────────────────────────────────────────────────────
 * 每块分配前面放 16 字节头（magic + 字节数），free 时校验 magic。
 * 目的是让"重复释放 / 释放别人的指针"在 host 上**当场炸**，
 * 而不是在真机上偶发一次看不懂的崩溃。 */
#define PF_HF_HDR_MAGIC 0x48464D45u /* "HFME" */

typedef struct {
    uint32_t magic;
    uint32_t pad;
    size_t bytes;
} pf_hf_hdr_t;

static void pf_hf_die(const char *why, const void *p)
{
    fprintf(stderr, "pf_host_fake: %s (%p)\n", why, p);
    fflush(stderr);
    abort();
}

static void *pf_hf_mem_alloc(size_t bytes)
{
    if (bytes == 0) {
        return NULL;
    }
    pf_hf_hdr_t *h = malloc(sizeof *h + bytes);
    if (h == NULL) {
        return NULL;
    }
    h->magic = PF_HF_HDR_MAGIC;
    h->pad = 0;
    h->bytes = bytes;
    return (char *)h + sizeof *h;
}

static void pf_hf_mem_free(void *p)
{
    if (p == NULL) {
        return; /* 契约：free(NULL) 必须无操作 */
    }
    pf_hf_hdr_t *h = (pf_hf_hdr_t *)((char *)p - sizeof(pf_hf_hdr_t));
    if (h->magic != PF_HF_HDR_MAGIC) {
        pf_hf_die("释放了一个不是 mem_alloc 给的指针（或重复释放）", p);
    }
    h->magic = 0;
    free(h);
}

/* ── 时间 ────────────────────────────────────────────────────────── */
static uint32_t pf_hf_clock_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    /* 故意按 uint32_t 截断：回绕是契约里写明的正常行为。 */
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void pf_hf_sleep_ms(uint32_t ms)
{
    if (ms == 0) {
        sched_yield(); /* 契约：0 表示让出 CPU 一次，不保证让给谁 */
        return;
    }
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;

    /* EINTR 之后必须补睡剩下的时间，否则"睡至少 ms"就不成立。 */
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        /* req 已被 nanosleep 改写成剩余时间 */
    }
}

/* ── 互斥 ────────────────────────────────────────────────────────── */
struct pf_mutex {
    pthread_mutex_t m;
};

static pf_mutex_t *pf_hf_mutex_create(void)
{
    pf_mutex_t *mx = calloc(1, sizeof *mx);
    if (mx == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&mx->m, NULL) != 0) {
        free(mx);
        return NULL;
    }
    return mx;
}

static int pf_hf_mutex_take(pf_mutex_t *mx, uint32_t timeout_ms)
{
    if (mx == NULL) {
        return -1; /* 契约：NULL 不崩溃，按"没拿到"处理 */
    }
    if (pthread_mutex_trylock(&mx->m) == 0) {
        return 0;
    }
    if (timeout_ms == 0) {
        return -1; /* 契约：0 = 只试一次 */
    }

    /* 相对超时：从**进入函数**那一刻起算。macOS 没有 pthread_mutex_timedlock，
     * 所以这里用"trylock + 睡 1ms"轮询。语义正确、精度 1ms、期间真的让出 CPU。 */
    uint32_t start = pf_hf_clock_ms();
    for (;;) {
        if ((uint32_t)(pf_hf_clock_ms() - start) >= timeout_ms) {
            return -1;
        }
        pf_hf_sleep_ms(1);
        if (pthread_mutex_trylock(&mx->m) == 0) {
            return 0;
        }
    }
}

static void pf_hf_mutex_give(pf_mutex_t *mx)
{
    if (mx == NULL) {
        return; /* 契约：NULL 无操作 */
    }
    (void)pthread_mutex_unlock(&mx->m);
}

/* ── 任务 ────────────────────────────────────────────────────────── */
struct pf_task {
    pthread_t th;
    pf_task_desc_t desc;
};

/* 有些 libc 不把 PTHREAD_STACK_MIN 放进 <limits.h>；给个保守下限，
 * 免得整个文件因为一个宏没定义就编不过。 */
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif

static void pf_hf_set_current_name(const char *name)
{
    if (name == NULL) {
        return;
    }
    char buf[16]; /* Linux 的线程名上限是 15 字节 + NUL */
    strncpy(buf, name, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
#if defined(__APPLE__)
    (void)pthread_setname_np(buf);
#elif defined(__linux__)
    (void)pthread_setname_np(pthread_self(), buf);
#endif
}

static void *pf_hf_task_trampoline(void *arg)
{
    struct pf_task *t = arg;
    pf_hf_set_current_name(t->desc.name);
    t->desc.entry(t->desc.arg);
    return NULL; /* 契约：entry 返回等于任务结束 */
}

static pf_task_t *pf_hf_task_create(const pf_task_desc_t *desc)
{
    if (desc == NULL || desc->entry == NULL) {
        return NULL;
    }
    if (desc->stack_bytes < PF_TASK_MIN_STACK) {
        return NULL; /* 契约：太小就拒绝，不许自动抬到最小栈 */
    }

    struct pf_task *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->desc = *desc; /* 拷一份：调用方可能在栈上、可能马上失效 */

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        free(t);
        return NULL;
    }

    size_t stack = (size_t)desc->stack_bytes;
    size_t page = 4096;
    stack = ((stack + page - 1) / page) * page;
    if (stack < (size_t)PTHREAD_STACK_MIN) {
        stack = (size_t)PTHREAD_STACK_MIN;
    }
    if (pthread_attr_setstacksize(&attr, stack) != 0 ||
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        pthread_attr_destroy(&attr);
        free(t);
        return NULL;
    }

    int rc = pthread_create(&t->th, &attr, pf_hf_task_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return NULL;
    }

    /* 故意不回收 t：任务是 detached 的，随时可能自己结束；如果这里让任务自己
     * free，返回给框架的句柄就成了悬空的。host-fake 用"不回收"换"句柄永远可读"，
     * 代价是每个任务泄漏几十字节——进程内测试可以接受。真机实现由平台管生命周期。 */
    return t;
}

static void pf_hf_task_exit_current(void)
{
    pthread_exit(NULL); /* 契约：不返回 */
}

/* ── 假串口（PF_CAP_UART 的参考实现） ────────────────────────────────
 * 它不碰任何真实设备：写进去的字节进 `s_uart_tx` 记账缓冲，读出来的字节来自
 * 测试用 `pf_hf_uart_feed()` 塞进去的 `s_uart_rx`。这样 host 上就能验：
 *   - "写了多少字节、内容对不对"（`pf_hf_uart_take()`）
 *   - "读的切片与超时行为"（`pf_hf_uart_feed()` + `uart_read()`）
 * 真机上这两件事只能验"没崩、返回码对"——对端有没有收到，要另一个人看。
 */
#define PF_HF_UART_RX_CAP 256u
#define PF_HF_UART_TX_CAP 4096u

/* 假串口的两个单向缓冲与打开状态。用途：让 host 上能验"写了多少、读到什么、
 * 切片与超时是否按契约走"，而不需要真硬件。
 * 谁写：`s_uart_rx` 由测试钩子 `pf_hf_uart_feed()` 写、被 `pf_hf_uart_read()` 消费；
 *       `s_uart_tx` 由 `pf_hf_uart_write()` 写、被 `pf_hf_uart_take()` 取走；
 *       `s_uart_opened` 由 open/close 写。
 * 并发约定：全部在 `s_uart_lock` 之内访问（测试主任务与 App 任务会同时碰它们）。 */
static pthread_mutex_t s_uart_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char s_uart_rx[PF_HF_UART_RX_CAP];
static size_t s_uart_rx_len;
static unsigned char s_uart_tx[PF_HF_UART_TX_CAP];
static size_t s_uart_tx_len;
static int s_uart_opened;

static int pf_hf_uart_open(uint32_t port, uint32_t baud)
{
    int rc = -1;

    if (port != 0u || baud == 0u) {
        return -1;
    }
    pthread_mutex_lock(&s_uart_lock);
    if (!s_uart_opened) {
        s_uart_opened = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&s_uart_lock);
    return rc;
}

static int pf_hf_uart_close(uint32_t port)
{
    int rc = -1;

    if (port != 0u) {
        return -1;
    }
    pthread_mutex_lock(&s_uart_lock);
    if (s_uart_opened) {
        s_uart_opened = 0;
        rc = 0;
    }
    pthread_mutex_unlock(&s_uart_lock);
    return rc;
}

static int pf_hf_uart_write(uint32_t port, const void *buf, size_t len, uint32_t timeout_ms)
{
    size_t room;
    size_t n;

    (void)timeout_ms;
    if (port != 0u || buf == NULL) {
        return -1;
    }
    pthread_mutex_lock(&s_uart_lock);
    if (!s_uart_opened) {
        pthread_mutex_unlock(&s_uart_lock);
        return -1;
    }
    room = PF_HF_UART_TX_CAP - s_uart_tx_len;
    n = (len < room) ? len : room;
    if (n > 0u) {
        memcpy(&s_uart_tx[s_uart_tx_len], buf, n);
        s_uart_tx_len += n;
    }
    pthread_mutex_unlock(&s_uart_lock);
    return (int)n; /* 满了返回 0：模拟"发送缓冲满、等一会儿再来" */
}

static int pf_hf_uart_read(uint32_t port, void *buf, size_t cap, uint32_t timeout_ms)
{
    size_t n;

    (void)timeout_ms; /* 假串口没有"等"，有就有、没有就 0 */
    if (port != 0u || buf == NULL || cap == 0u) {
        return -1;
    }
    pthread_mutex_lock(&s_uart_lock);
    if (!s_uart_opened) {
        pthread_mutex_unlock(&s_uart_lock);
        return -1;
    }
    n = (cap < s_uart_rx_len) ? cap : s_uart_rx_len;
    if (n > 0u) {
        memcpy(buf, s_uart_rx, n);
        memmove(s_uart_rx, &s_uart_rx[n], s_uart_rx_len - n);
        s_uart_rx_len -= n;
    }
    pthread_mutex_unlock(&s_uart_lock);
    return (int)n;
}

static const pf_uart_ops_t hf_uart_ops = {
    .version = PF_UART_OPS_VERSION,
    .count = 1u,
    .open = pf_hf_uart_open,
    .close = pf_hf_uart_close,
    .write = pf_hf_uart_write,
    .read = pf_hf_uart_read,
};

void pf_hf_uart_feed(const void *data, size_t len)
{
    size_t room;

    if (data == NULL) {
        return;
    }
    pthread_mutex_lock(&s_uart_lock);
    room = PF_HF_UART_RX_CAP - s_uart_rx_len;
    if (len > room) {
        len = room;
    }
    memcpy(&s_uart_rx[s_uart_rx_len], data, len);
    s_uart_rx_len += len;
    pthread_mutex_unlock(&s_uart_lock);
}

size_t pf_hf_uart_take(void *buf, size_t cap)
{
    size_t n;

    if (buf == NULL) {
        return 0u;
    }
    pthread_mutex_lock(&s_uart_lock);
    n = (cap < s_uart_tx_len) ? cap : s_uart_tx_len;
    if (n > 0u) {
        memcpy(buf, s_uart_tx, n);
        memmove(s_uart_tx, &s_uart_tx[n], s_uart_tx_len - n);
        s_uart_tx_len -= n;
    }
    pthread_mutex_unlock(&s_uart_lock);
    return n;
}

void pf_hf_uart_reset(void)
{
    pthread_mutex_lock(&s_uart_lock);
    s_uart_opened = 0;
    s_uart_rx_len = 0u;
    s_uart_tx_len = 0u;
    pthread_mutex_unlock(&s_uart_lock);
}

/* ── 能力与表 ────────────────────────────────────────────────────── */
static uint32_t pf_hf_caps(void)
{
    return PF_CAP_UART; /* 这块"板"只有假串口；其他能力由测试自己往注册表里塞 */
}

void pf_platform_boot(void)
{
    /* 板级启动钩子：把能力实现交上去。重复注册返回 -1 是刻意的（不覆盖），
     * 这里忽略返回值——`fw_reset()` 之后会再调一次，此刻表是空的。 */
    (void)fw_cap_register(PF_CAP_UART, &hf_uart_ops);
}

const pf_ops_t pf_host_fake_ops = {
    .version = PF_OPS_VERSION,
    .caps = pf_hf_caps,
    .mem_alloc = pf_hf_mem_alloc,
    .mem_free = pf_hf_mem_free,
    .clock_ms = pf_hf_clock_ms,
    .sleep_ms = pf_hf_sleep_ms,
    .mutex_create = pf_hf_mutex_create,
    .mutex_take = pf_hf_mutex_take,
    .mutex_give = pf_hf_mutex_give,
    .task_create = pf_hf_task_create,
    .task_exit_current = pf_hf_task_exit_current,
};

const pf_ops_t *pf_host_fake_ops_get(void)
{
    return &pf_host_fake_ops;
}

const pf_ops_t *pf_platform_ops(void)
{
    return &pf_host_fake_ops;
}

void pf_platform_log(const char *line)
{
    if (line == NULL) {
        return;
    }
    /* stderr 不缓冲：进程被 abort() 掉的时候，缓冲区里的日志会一起消失。 */
    fprintf(stderr, "%s\n", line);
}
