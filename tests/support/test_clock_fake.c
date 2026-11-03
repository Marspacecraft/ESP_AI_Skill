/* tests/support/test_clock_fake.c —— 夹具自己的验收（步骤 2.5 的前半）。
 *
 * 夹具是承重墙，所以它自己也得有一条"会红"的用例。这里验四件事：
 *   1. 虚拟时间只由夹具推进，与真实时间无关；
 *   2. `sleep_ms` 不真的睡（用真表的 `clock_ms` 量真实耗时）；
 *   3. 切片长度被如实记录（2.4 要靠它断言"每片 ≤ 20ms"）；
 *   4. `sleep_ms(0)` 算一次让出，但不推进时间。
 */
#include "tinytest.h"

#include "platform/host-fake/pf_host_fake.h"
#include "tests/support/clock_fake.h"

static const pf_ops_t *g_real;

TT_CASE(starts_at_zero_and_advances_by_sleep)
{
    const pf_ops_t *cf;

    clock_fake_install(g_real);
    cf = clock_fake_ops();
    TT_ASSERT_NOT_NULL(cf);
    TT_ASSERT_INT(0, (int)cf->clock_ms());

    cf->sleep_ms(20);
    cf->sleep_ms(5);
    TT_ASSERT_INT(25, (int)cf->clock_ms());
    TT_ASSERT_INT(25, (int)clock_fake_now());
}

TT_CASE(sleep_does_not_really_sleep)
{
    const pf_ops_t *cf;
    uint32_t before;
    uint32_t after;

    clock_fake_install(g_real);
    clock_fake_reset_stats();
    cf = clock_fake_ops();

    before = g_real->clock_ms();
    cf->sleep_ms(500); /* 真睡就是半秒；虚拟只拨 500ms */
    after = g_real->clock_ms();

    /* 真实耗时应当远小于 500ms；给 100ms 余量防止慢机器抖动。 */
    TT_ASSERT_TRUE((uint32_t)(after - before) < 100u);
    TT_ASSERT_INT(500, (int)cf->clock_ms());
}

TT_CASE(slices_are_recorded)
{
    const pf_ops_t *cf;

    clock_fake_install(g_real);
    clock_fake_reset_stats();
    cf = clock_fake_ops();

    cf->sleep_ms(20);
    cf->sleep_ms(7);
    cf->sleep_ms(13);

    TT_ASSERT_INT(3, (int)clock_fake_sleep_calls());
    TT_ASSERT_INT(40, (int)clock_fake_sleep_total_ms());
    TT_ASSERT_INT(20, (int)clock_fake_max_slice_ms());
    TT_ASSERT_INT(20, (int)clock_fake_slice(0));
    TT_ASSERT_INT(7, (int)clock_fake_slice(1));
    TT_ASSERT_INT(13, (int)clock_fake_slice(2));
    TT_ASSERT_INT(0, (int)clock_fake_slice(3));
}

TT_CASE(zero_sleep_counts_but_does_not_advance)
{
    const pf_ops_t *cf;

    clock_fake_install(g_real);
    clock_fake_reset_stats();
    cf = clock_fake_ops();

    cf->sleep_ms(0);
    TT_ASSERT_INT(1, (int)clock_fake_sleep_calls());
    TT_ASSERT_INT(0, (int)cf->clock_ms());
    TT_ASSERT_INT(0, (int)clock_fake_sleep_total_ms());
}

TT_CASE(advance_jumps_time_without_sleeping)
{
    const pf_ops_t *cf;

    clock_fake_install(g_real);
    clock_fake_reset_stats();
    cf = clock_fake_ops();

    clock_fake_advance(250);
    TT_ASSERT_INT(250, (int)cf->clock_ms());
    TT_ASSERT_INT(0, (int)clock_fake_sleep_calls());
}

TT_CASE(install_keeps_other_members_real)
{
    const pf_ops_t *cf;

    clock_fake_install(g_real);
    cf = clock_fake_ops();

    /* 分配、锁、任务都还是真表的实现——否则测出来的不是框架的行为。 */
    TT_ASSERT_TRUE(cf->mem_alloc == g_real->mem_alloc);
    TT_ASSERT_TRUE(cf->mem_free == g_real->mem_free);
    TT_ASSERT_TRUE(cf->mutex_create == g_real->mutex_create);
    TT_ASSERT_TRUE(cf->task_create == g_real->task_create);
    TT_ASSERT_INT((int)g_real->caps(), (int)cf->caps());
}

int main(void)
{
    g_real = pf_host_fake_ops_get();
    TT_PROGRAM("test_clock_fake");
    TT_RUN(starts_at_zero_and_advances_by_sleep);
    TT_RUN(sleep_does_not_really_sleep);
    TT_RUN(slices_are_recorded);
    TT_RUN(zero_sleep_counts_but_does_not_advance);
    TT_RUN(advance_jumps_time_without_sleeping);
    TT_RUN(install_keeps_other_members_real);
    return tt_summary();
}
