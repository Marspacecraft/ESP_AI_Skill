/* tests/framework/test_cap.c —— `cap_available()` 与能力注册表的边界。
 *
 * `cap_available()` 的回答 = **硬件报了 AND 软件注册了**。这两半各自都会被
 * "顺手简化"掉，所以分别钉住：
 *
 *   1. 平台表没绑时回答"没有"，而不是崩、也不是"可能有"。
 *   2. 平台报了位但板子没注册实现 → false（"查得到硬件"不等于"调得动它"）。
 *   3. 平台没报的位一律 false——即便注册表里有实现（板子报了假能力也不认）。
 *   4. 传 0、传多位组合、传保留位，一律 false：静默按位或会造出"只有屏的板上
 *      说触摸可用"这种假象。
 *   5. 注册表只接受九个明确位号中的一个 + 非空指针，且不接受重复注册。
 */
#include "tinytest.h"

#include "api/af_api/cap.h"
#include "framework/include/fw_app.h"
#include "framework/src/fw_internal.h"
#include "platform/host-fake/pf_host_fake.h"
#include "platform/port/pf_cap.h"
#include "platform/port/pf_ops.h"

static uint32_t g_caps;

static uint32_t fake_caps(void)
{
    return g_caps;
}

/* 板子交上来的"实现表"。本文件只验注册表本身，所以内容不重要；
 * 真正的形状（`pf_disp_ops_t`）等第一个能力头落地时才出现。 */
static const uint32_t g_disp_impl = 0xD15Bu;
static const uint32_t g_key_impl = 0x4B45u;

/* 拷一份 host-fake 的真表，只换 `caps`——其余八项仍是真实实现，
 * 这样 `fw_ops_check()` 的完整性校验也顺便被走了一遍。 */
static pf_ops_t g_ops;

static cm_err_t bind_with(uint32_t caps)
{
    fw_reset();
    g_ops = pf_host_fake_ops;
    g_ops.caps = fake_caps;
    g_caps = caps;
    return fw_platform_bind(&g_ops);
}

TT_CASE(no_platform_means_no_capability)
{
    fw_reset();

    TT_ASSERT_FALSE(cap_available(CAP_DISPLAY));
    TT_ASSERT_FALSE(cap_available(CAP_RTC));
}

TT_CASE(caps_zero_reports_nothing)
{
    TT_ASSERT_INT(CM_OK, bind_with(0u));

    TT_ASSERT_FALSE(cap_available(CAP_DISPLAY));
    TT_ASSERT_FALSE(cap_available(CAP_DISPLAY_PARTIAL));
    TT_ASSERT_FALSE(cap_available(CAP_TOUCH));
    TT_ASSERT_FALSE(cap_available(CAP_KEY));
    TT_ASSERT_FALSE(cap_available(CAP_STORAGE));
    TT_ASSERT_FALSE(cap_available(CAP_NET));
    TT_ASSERT_FALSE(cap_available(CAP_AUDIO_OUT));
    TT_ASSERT_FALSE(cap_available(CAP_AUDIO_IN));
    TT_ASSERT_FALSE(cap_available(CAP_RTC));
}

TT_CASE(reported_without_implementation_is_not_available)
{
    /* 板子说"我有屏"，但没交实现表。这正是"查得到硬件 ≠ 调得动它"。 */
    TT_ASSERT_INT(CM_OK, bind_with(CAP_DISPLAY));

    TT_ASSERT_TRUE(fw_caps_reported(CAP_DISPLAY));
    TT_ASSERT_TRUE(fw_cap_ops(CAP_DISPLAY) == NULL);
    TT_ASSERT_FALSE(cap_available(CAP_DISPLAY));
}

TT_CASE(registered_and_reported_is_available)
{
    TT_ASSERT_INT(CM_OK, bind_with(CAP_DISPLAY | CAP_KEY));
    TT_ASSERT_INT(0, fw_cap_register(CAP_DISPLAY, &g_disp_impl));

    TT_ASSERT_TRUE(cap_available(CAP_DISPLAY));
    TT_ASSERT_FALSE(cap_available(CAP_KEY)); /* 报了但没注册 */
}

TT_CASE(implementation_without_reported_bit_is_not_available)
{
    /* 注册了实现、平台却没报这一位：板子报了假能力，按"没有"处理。 */
    TT_ASSERT_INT(CM_OK, bind_with(0u));
    TT_ASSERT_INT(0, fw_cap_register(CAP_TOUCH, &g_key_impl));

    TT_ASSERT_TRUE(fw_cap_ops(CAP_TOUCH) != NULL);
    TT_ASSERT_FALSE(cap_available(CAP_TOUCH));
}

TT_CASE(invalid_arguments_answer_false)
{
    TT_ASSERT_INT(CM_OK, bind_with(CAP_DISPLAY | CAP_KEY));
    TT_ASSERT_INT(0, fw_cap_register(CAP_DISPLAY, &g_disp_impl));
    TT_ASSERT_INT(0, fw_cap_register(CAP_KEY, &g_key_impl));

    TT_ASSERT_FALSE(cap_available(0u));                        /* 0 不是能力位 */
    TT_ASSERT_FALSE(cap_available(CAP_DISPLAY | CAP_KEY));     /* 多位组合：不猜 */
    TT_ASSERT_FALSE(cap_available(1u << 10));                  /* 保留位 */
    TT_ASSERT_FALSE(cap_available(1u << 31));                  /* 保留位 */
}

TT_CASE(registration_rejects_invalid_input)
{
    TT_ASSERT_INT(CM_OK, bind_with(CAP_DISPLAY | CAP_KEY));

    TT_ASSERT_INT(-1, fw_cap_register(0u, &g_disp_impl));                    /* 0 */
    TT_ASSERT_INT(-1, fw_cap_register(1u << 10, &g_disp_impl));              /* 保留位 */
    TT_ASSERT_INT(-1, fw_cap_register(CAP_DISPLAY | CAP_KEY, &g_disp_impl)); /* 多位 */
    TT_ASSERT_INT(-1, fw_cap_register(CAP_DISPLAY, NULL));                   /* 空实现 */
    TT_ASSERT_INT(0, fw_cap_register(CAP_DISPLAY, &g_disp_impl));
    TT_ASSERT_INT(-1, fw_cap_register(CAP_DISPLAY, &g_key_impl)); /* 重复：不覆盖 */
    TT_ASSERT_TRUE(fw_cap_ops(CAP_DISPLAY) == &g_disp_impl);      /* 还是第一份 */
}

TT_CASE(reset_clears_registrations)
{
    TT_ASSERT_INT(CM_OK, bind_with(CAP_DISPLAY));
    TT_ASSERT_INT(0, fw_cap_register(CAP_DISPLAY, &g_disp_impl));
    TT_ASSERT_TRUE(cap_available(CAP_DISPLAY));

    fw_reset(); /* 平台表也一起没了 */

    TT_ASSERT_TRUE(fw_cap_ops(CAP_DISPLAY) == NULL);
    TT_ASSERT_FALSE(cap_available(CAP_DISPLAY));
}

TT_CASE(forget_clears_registrations)
{
    TT_ASSERT_INT(CM_OK, bind_with(CAP_DISPLAY));
    TT_ASSERT_INT(0, fw_cap_register(CAP_DISPLAY, &g_disp_impl));
    TT_ASSERT_TRUE(cap_available(CAP_DISPLAY));

    fw_cap_forget();

    TT_ASSERT_TRUE(fw_cap_ops(CAP_DISPLAY) == NULL);
    TT_ASSERT_FALSE(cap_available(CAP_DISPLAY)); /* 硬件还报着，实现没了 */
}

int main(void)
{
    TT_PROGRAM("test_cap");

    TT_RUN(no_platform_means_no_capability);
    TT_RUN(caps_zero_reports_nothing);
    TT_RUN(reported_without_implementation_is_not_available);
    TT_RUN(registered_and_reported_is_available);
    TT_RUN(implementation_without_reported_bit_is_not_available);
    TT_RUN(invalid_arguments_answer_false);
    TT_RUN(registration_rejects_invalid_input);
    TT_RUN(reset_clears_registrations);
    TT_RUN(forget_clears_registrations);
    return tt_summary();
}
