/* tests/platform/test_stack_contract.c —— 两个"最小值"必须是同一个方向。
 *
 * `APP_STACK_MIN`（`fw_app.h`，App 侧：小于它的清单直接不启动）与
 * `PF_TASK_MIN_STACK`（`pf_ops.h`，平台侧：小于它的栈平台拒绝建任务）住在
 * 两个互不 include 的头里（D2 不允许），数字却必须满足一条关系：
 *
 *     任何被 App 侧接受的栈，都必须被平台侧接受
 *
 * 即 `APP_STACK_MIN >= PF_TASK_MIN_STACK`。写反了会得到一个很隐蔽的现场：
 * 清单校验通过、任务创建失败、App 起来就是 FAILED，而错误码看起来像"内存不够"。
 *
 * 这条用 `_Static_assert` 钉在编译期，所以它不需要运行也能生效——运行时那两条
 * 断言只是把"我以为的关系"写出来给人看。
 */
#include "tinytest.h"
#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

_Static_assert(APP_STACK_MIN >= PF_TASK_MIN_STACK,
               "APP_STACK_MIN 必须 >= PF_TASK_MIN_STACK：否则清单收下的栈，平台建不出任务");
_Static_assert(PF_OPS_VERSION == 1u, "平台表版本变了就得同步改这个测试");

TT_CASE(app_stack_floor_is_not_below_platform_floor)
{
    TT_ASSERT_TRUE(APP_STACK_MIN >= PF_TASK_MIN_STACK);
}

TT_CASE(platform_table_version_is_the_one_we_compiled_against)
{
    TT_ASSERT_INT(1u, PF_OPS_VERSION);
}

int main(void)
{
    TT_PROGRAM("test_stack_contract");

    TT_RUN(app_stack_floor_is_not_below_platform_floor);
    TT_RUN(platform_table_version_is_the_one_we_compiled_against);
    return tt_summary();
}
