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

/* ── 内存 ──────────────────────────────────────────────────────────
 * 每块分配前面放 16 字节头（magic + 字节数），free 时校验 magic。
 * 目的是让"重复释放 / 释放别人的指针"在 host 上**当场炸**，
 * 而不是在真机上偶发一次看不懂的崩溃。 */
#define HF_HDR_MAGIC 0x48464D45u /* "HFME" */

typedef struct {
    uint32_t magic;
    uint32_t pad;
    size_t bytes;
} hf_hdr_t;

static void hf_die(const char *why, const void *p)
{
    fprintf(stderr, "pf_host_fake: %s (%p)\n", why, p);
    fflush(stderr);
    abort();
}

static void *hf_mem_alloc(size_t bytes)
{
    if (bytes == 0) {
        return NULL;
    }
    hf_hdr_t *h = malloc(sizeof *h + bytes);
    if (h == NULL) {
        return NULL;
    }
    h->magic = HF_HDR_MAGIC;
    h->pad = 0;
    h->bytes = bytes;
    return (char *)h + sizeof *h;
}

static void hf_mem_free(void *p)
{
    if (p == NULL) {
        return; /* 契约：free(NULL) 必须无操作 */
    }
    hf_hdr_t *h = (hf_hdr_t *)((char *)p - sizeof(hf_hdr_t));
    if (h->magic != HF_HDR_MAGIC) {
        hf_die("释放了一个不是 mem_alloc 给的指针（或重复释放）", p);
    }
    h->magic = 0;
    free(h);
}

/* ── 时间 ────────────────────────────────────────────────────────── */
static uint32_t hf_clock_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    /* 故意按 uint32_t 截断：回绕是契约里写明的正常行为。 */
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void hf_sleep_ms(uint32_t ms)
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

static pf_mutex_t *hf_mutex_create(void)
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

static int hf_mutex_take(pf_mutex_t *mx, uint32_t timeout_ms)
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
    uint32_t start = hf_clock_ms();
    for (;;) {
        if ((uint32_t)(hf_clock_ms() - start) >= timeout_ms) {
            return -1;
        }
        hf_sleep_ms(1);
        if (pthread_mutex_trylock(&mx->m) == 0) {
            return 0;
        }
    }
}

static void hf_mutex_give(pf_mutex_t *mx)
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

static void hf_set_current_name(const char *name)
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

static void *hf_task_trampoline(void *arg)
{
    struct pf_task *t = arg;
    hf_set_current_name(t->desc.name);
    t->desc.entry(t->desc.arg);
    return NULL; /* 契约：entry 返回等于任务结束 */
}

static pf_task_t *hf_task_create(const pf_task_desc_t *desc)
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

    int rc = pthread_create(&t->th, &attr, hf_task_trampoline, t);
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

static void hf_task_exit_current(void)
{
    pthread_exit(NULL); /* 契约：不返回 */
}

/* ── 能力与表 ────────────────────────────────────────────────────── */
static uint32_t hf_caps(void)
{
    return 0; /* 这块"板"没有硬件；服务由测试自己注册假实现 */
}

const pf_ops_t pf_host_fake_ops = {
    .version = PF_OPS_VERSION,
    .caps = hf_caps,
    .mem_alloc = hf_mem_alloc,
    .mem_free = hf_mem_free,
    .clock_ms = hf_clock_ms,
    .sleep_ms = hf_sleep_ms,
    .mutex_create = hf_mutex_create,
    .mutex_take = hf_mutex_take,
    .mutex_give = hf_mutex_give,
    .task_create = hf_task_create,
    .task_exit_current = hf_task_exit_current,
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
