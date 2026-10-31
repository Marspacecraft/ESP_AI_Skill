/*
 * tests/common/test_cm_types.c —— cm_types.h / cm_port.h 的用例。
 *
 * 只验"类型宽度与通用宏"这两类容易在换工具链时悄悄变形的假设；
 * 编译期断言与运行期断言重叠是有意的：前者挡编译，后者挡住"头文件被
 * 条件编译改窄"这种编译期断言也挡不住的情况。
 */

#include "cm_types.h"

#include "tinytest.h"

struct cm_types_sample {
    int value;
    char tag;
};

TT_CASE(width_aliases_match_stdint)
{
    TT_ASSERT_INT(1, sizeof(cm_u8));
    TT_ASSERT_INT(2, sizeof(cm_u16));
    TT_ASSERT_INT(4, sizeof(cm_u32));
    TT_ASSERT_INT(8, sizeof(cm_u64));
    TT_ASSERT_INT(1, sizeof(cm_i8));
    TT_ASSERT_INT(2, sizeof(cm_i16));
    TT_ASSERT_INT(4, sizeof(cm_i32));
    TT_ASSERT_INT(8, sizeof(cm_i64));
}

TT_CASE(width_aliases_are_exact_width)
{
    cm_u8 a = (cm_u8)255;
    cm_u16 b = (cm_u16)65535;
    cm_i8 c = (cm_i8)-1;
    cm_i32 d = (cm_i32)-2147483647 - 1;

    TT_ASSERT_INT(255, a);
    TT_ASSERT_INT(65535, b);
    TT_ASSERT_INT(-1, c);
    TT_ASSERT_INT(-2147483647 - 1, d);
}

TT_CASE(array_size_counts_elements)
{
    int numbers[7];
    char bytes[3];

    TT_ASSERT_INT(7, CM_ARRAY_SIZE(numbers));
    TT_ASSERT_INT(3, CM_ARRAY_SIZE(bytes));
}

TT_CASE(min_max_pick_expected)
{
    TT_ASSERT_INT(3, CM_MIN(3, 9));
    TT_ASSERT_INT(9, CM_MAX(3, 9));
    TT_ASSERT_INT(-2, CM_MIN(-2, 5));
    TT_ASSERT_INT(5, CM_MAX(-2, 5));
}

TT_CASE(container_of_recovers_host)
{
    struct cm_types_sample item = {42, 'x'};
    struct cm_types_sample *recovered =
        CM_CONTAINER_OF(&item.tag, struct cm_types_sample, tag);

    TT_ASSERT(recovered == &item);
    TT_ASSERT_INT(42, recovered->value);
    TT_ASSERT_INT('x', recovered->tag);
}

TT_CASE(align_up_down_round_to_power_of_two)
{
    TT_ASSERT_INT(16, CM_ALIGN_UP(13, 8));
    TT_ASSERT_INT(16, CM_ALIGN_UP(16, 8));
    TT_ASSERT_INT(8, CM_ALIGN_DOWN(13, 8));
    TT_ASSERT_INT(16, CM_ALIGN_DOWN(16, 8));
    TT_ASSERT_INT(64, CM_ALIGN_UP(33, 32));
}

TT_CASE(port_macros_are_usable)
{
    int hot = 1;

    TT_ASSERT(CM_LIKELY(hot));
    /* CM_UNLIKELY 只影响分支预测，返回值仍是表达式本身的值 */
    TT_ASSERT(CM_UNLIKELY(hot));
    TT_ASSERT(!CM_UNLIKELY(!hot));
    TT_ASSERT_INT(4, CM_ALIGNOF(cm_u32));
}

int main(void)
{
    TT_PROGRAM("test_cm_types");

    TT_RUN(width_aliases_match_stdint);
    TT_RUN(width_aliases_are_exact_width);
    TT_RUN(array_size_counts_elements);
    TT_RUN(min_max_pick_expected);
    TT_RUN(container_of_recovers_host);
    TT_RUN(align_up_down_round_to_power_of_two);
    TT_RUN(port_macros_are_usable);

    return tt_summary();
}
