/* tests/support/test_smoke.c —— 工作台冒烟：证明确实能编、能跑、能报数。
 * 它不测任何业务逻辑，只测"测试本身是活的"。步骤 0.3。
 */
#include "tinytest.h"

TT_CASE(ints_compare)
{
    TT_ASSERT_INT(1, 1);
}

TT_CASE(strings_compare)
{
    TT_ASSERT_STR("esp-ai-skill", "esp-ai-skill");
}

TT_CASE(true_is_true)
{
    TT_ASSERT_TRUE(1 == 1);
    TT_ASSERT_FALSE(1 == 2);
    TT_ASSERT_NOT_NULL("x");
}

int main(void)
{
    TT_PROGRAM("test_smoke");
    TT_RUN(ints_compare);
    TT_RUN(strings_compare);
    TT_RUN(true_is_true);
    return tt_summary();
}
