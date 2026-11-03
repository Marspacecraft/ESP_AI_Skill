/* tests/support/clock_fake.h —— 虚拟时钟夹具（步骤 2.5 的前半）。
 *
 * 为什么要有它：伪阻塞的验收是"切片等待 + 每片查取消位"，如果每片都真睡，
 * 一条用例就是几百毫秒，十条就是几秒，而且时长会随机器抖动——测试一旦靠
 * 真实时间，失败就分不清是逻辑错了还是机器慢了。
 *
 * 做法：把一张真实平台表的 `clock_ms` / `sleep_ms` 换掉，其余九项原样转发。
 * 于是框架看到的"时间"由夹具说了算：`sleep_ms(20)` 只把虚拟时间往前拨 20，
 * 不真的睡；每次调用的切片长度被记下来，测试可以直接断言"每片 ≤ 20ms"。
 *
 * **为什么是 header-only（一整份实现都在这）**：
 * `build.sh` 只链 `common/src`、`framework/src`、`platform/`、`apps/` 下的源文件，
 * `tests/support/` 不在其中；而 `build.sh` 属于"只有人能改"。所以夹具要么是
 * header-only，要么就得请人改构建清单。测试替身本来就小，选前者。
 * 后果：每个 include 它的编译单元各有一份状态——一个二进制里只用一份，够用。
 *
 * 单例：`pf_ops_t` 的回调没有 user 参数，夹具只能有一个全局实例。
 *       这是 host 测试的既定形态（单线程、顺序执行），**不要并发使用**。
 *
 * 线程与中断：仅测试用，非线程安全。
 */
#ifndef ESP_AI_SKILL_CLOCK_FAKE_H
#define ESP_AI_SKILL_CLOCK_FAKE_H

#include <stdint.h>
#include <string.h>

#include "platform/port/pf_ops.h"

/** 最多记录多少次 sleep 调用；超出只计数不记录（防止用例写崩把内存写穿）。 */
#define CLOCK_FAKE_MAX_SLICES 64u

/** `-Wunused` 在本仓是错误，所以每个只用一次的符号都标上。 */
#if defined(__GNUC__) || defined(__clang__)
#define CF_UNUSED __attribute__((unused))
#else
#define CF_UNUSED
#endif

CF_UNUSED static struct {
    pf_ops_t ops;      /* 给框架看的表：除时钟两项外与真表一致 */
    uint32_t now_ms;   /* 虚拟时间 */
    uint32_t sleep_calls;
    uint32_t sleep_total_ms;
    uint32_t max_slice_ms;
    uint32_t slices[CLOCK_FAKE_MAX_SLICES];
    uint32_t slice_count;
    int installed;
} g_cf;

/* 虚拟时钟：只返回夹具自己的时间，与真实 `CLOCK_MONOTONIC` 无关。 */
CF_UNUSED static uint32_t cf_clock_ms(void)
{
    return g_cf.now_ms;
}

/* 假睡：把虚拟时间往前拨 ms，记一笔切片，立即返回。
 * `ms==0` 的语义是"让出一次 CPU"，不推进时间。 */
CF_UNUSED static void cf_sleep_ms(uint32_t ms)
{
    g_cf.sleep_calls++;
    if (ms == 0u) {
        return;
    }
    if (g_cf.slice_count < CLOCK_FAKE_MAX_SLICES) {
        g_cf.slices[g_cf.slice_count] = ms;
    }
    if (g_cf.slice_count < 0xFFFFFFFFu) {
        g_cf.slice_count++;
    }
    if (ms > g_cf.max_slice_ms) {
        g_cf.max_slice_ms = ms;
    }
    g_cf.sleep_total_ms += ms;
    g_cf.now_ms += ms;
}

/** 用真实表初始化夹具：`real` 的九项会被转发，只有时钟两项被替换。
 *  同时把虚拟时间归零、清空切片记录。`real` 不得为 NULL。 */
CF_UNUSED static void clock_fake_install(const pf_ops_t *real)
{
    (void)memset(&g_cf, 0, sizeof g_cf);
    if (real == NULL) {
        g_cf.installed = 0;
        return;
    }
    /* 整表拷贝：版本、caps、九项都继承过来，只有下面两行被替换。 */
    g_cf.ops = *real;
    g_cf.ops.clock_ms = cf_clock_ms;
    g_cf.ops.sleep_ms = cf_sleep_ms;
    g_cf.installed = 1;
}

/** 夹具自己的平台表（版本号、caps、其余成员都来自 `real`）；未装时返回 NULL。 */
CF_UNUSED static const pf_ops_t *clock_fake_ops(void)
{
    return g_cf.installed ? &g_cf.ops : NULL;
}

/** 外部把虚拟时间往前拨（模拟"两次调用之间过了很久"）。 */
CF_UNUSED static void clock_fake_advance(uint32_t ms)
{
    g_cf.now_ms += ms;
}

/** 当前虚拟时间。 */
CF_UNUSED static uint32_t clock_fake_now(void)
{
    return g_cf.now_ms;
}

/** 清空切片记录（时间不重置）。每个用例开头调一次。 */
CF_UNUSED static void clock_fake_reset_stats(void)
{
    g_cf.sleep_calls = 0u;
    g_cf.sleep_total_ms = 0u;
    g_cf.max_slice_ms = 0u;
    g_cf.slice_count = 0u;
    (void)memset(g_cf.slices, 0, sizeof g_cf.slices);
}

/** 被调用的 sleep 次数（含 `sleep_ms(0)`）。 */
CF_UNUSED static uint32_t clock_fake_sleep_calls(void)
{
    return g_cf.sleep_calls;
}

/** 被累计"睡"掉的虚拟毫秒数。 */
CF_UNUSED static uint32_t clock_fake_sleep_total_ms(void)
{
    return g_cf.sleep_total_ms;
}

/** 最长的一次切片（毫秒）；没调用过返回 0。 */
CF_UNUSED static uint32_t clock_fake_max_slice_ms(void)
{
    return g_cf.max_slice_ms;
}

/** 第 index 次切片的长度；越界返回 0。 */
CF_UNUSED static uint32_t clock_fake_slice(uint32_t index)
{
    if (index >= CLOCK_FAKE_MAX_SLICES) {
        return 0u;
    }
    return g_cf.slices[index];
}

#endif /* ESP_AI_SKILL_CLOCK_FAKE_H */
