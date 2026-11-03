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
#include "api/af_api/cap.h"
#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

_Static_assert(APP_STACK_MIN >= PF_TASK_MIN_STACK,
               "APP_STACK_MIN 必须 >= PF_TASK_MIN_STACK：否则清单收下的栈，平台建不出任务");
_Static_assert(PF_OPS_VERSION == 1u, "平台表版本变了就得同步改这个测试");

/* 第三对：能力位。
 *
 * `api/af_api/cap.h` 的 `CAP_*` 与 `platform/port/pf_ops.h` 的 `PF_CAP_*` 是
 * **同一张位图的两次声明**——App 读不到 `pf_ops.h`（link 行不给 tag 1/2 读
 * `platform/`），所以只能重声明一次。位号一改而另一边忘了改，App 就会查到错的位，
 * 而且**不会报错**：`cap_available(CAP_TOUCH)` 返回 true 而板上根本没有触摸。
 * 这类错误只能靠编译期钉住，所以九个位逐一断言。 */
_Static_assert(CAP_DISPLAY == PF_CAP_DISPLAY, "能力位 0 不一致");
_Static_assert(CAP_DISPLAY_PARTIAL == PF_CAP_DISPLAY_PARTIAL, "能力位 1 不一致");
_Static_assert(CAP_TOUCH == PF_CAP_TOUCH, "能力位 2 不一致");
_Static_assert(CAP_KEY == PF_CAP_KEY, "能力位 3 不一致");
_Static_assert(CAP_STORAGE == PF_CAP_STORAGE, "能力位 4 不一致");
_Static_assert(CAP_NET == PF_CAP_NET, "能力位 5 不一致");
_Static_assert(CAP_AUDIO_OUT == PF_CAP_AUDIO_OUT, "能力位 6 不一致");
_Static_assert(CAP_AUDIO_IN == PF_CAP_AUDIO_IN, "能力位 7 不一致");
_Static_assert(CAP_RTC == PF_CAP_RTC, "能力位 8 不一致");

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
