/* framework/src/fw_cap.c —— 能力查询（实现）。
 *
 * 回答的是"**这一样现在能调吗**"，由两件事合起来判：
 *
 *   1. 平台表上的 `caps()` 报了这一位（硬件在）；
 *   2. 板子通过 `fw_cap_register()` 交了实现（软件能调）。
 *
 * 只有 (1) 就回答"能"，等于把"查得到硬件"和"调得动它"混为一谈——那是
 * `pf_ops.h` 头注释专门警告过的一类假信号。只有 (2) 就更简单：板子报了假能力。
 *
 * 薄的正是需要的：App 不该知道 `pf_ops_t` 长什么样，也不该自己缓存能力位——
 * 缓存了就要回答"什么时候失效"，而答案是"永不失效"，那就没有理由缓存。
 *
 * 为什么"多位组合"返回 false 而不是按位或：
 * 这个接口的回答是"**这一样**有没有"，传进来两位说明调用方想要的不是一个是非，
 * 而是两组不同处置的逻辑。静默按位或会让 `cap_available(CAP_DISPLAY | CAP_TOUCH)`
 * 在只有屏的板上返回 true，然后代码去摸触摸——那是最难查的一类现场。
 */
#include "framework/src/fw_internal.h"

bool cap_available(uint32_t bit)
{
    if (!fw_caps_reported(bit)) {
        return false; /* 平台表还没绑、0、多位组合、保留位、硬件没报：都在这里回落 */
    }
    return fw_cap_ops(bit) != NULL;
}
