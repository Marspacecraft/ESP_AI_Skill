/* framework/src/fw_cap_reg.c —— 能力注册表（板子把实现交给框架的地方）。
 *
 * 这张表回答的问题只有一个："这个能力位，软件能调吗？"
 *
 *   `caps()` 回答"硬件在不在"（平台表上的一个位），本表回答"软件有没有实现"。
 *   两者都成立才算可用——只有硬件位而没有实现，App 一调就是空指针；只有实现而
 *   没有硬件位，说明板子报了假能力。两个方向都是静默错误，所以在
 *   `cap_available()` 里用"与"把它们合起来判。
 *
 * 为什么用固定数组 + 位号到下标的显式映射，而不是哈希表：
 *
 *   能力位是**编译期已知的一组值**，映射写死就没有"查不到"这种状态；而哈希表
 *   会引入"两个位落到同一槽"的可能性，那正是我们最不想在能力查询里看到的错误
 *   形态。
 *
 * 为什么注册用原子写、查询用原子读：
 *
 *   注册发生在 boot 阶段（单线程），查询发生在任意任务里。用 RELEASE/ACQUIRE
 *   保证"读到的指针一定指向已经初始化完的表"——板子先填好 `ops` 再注册，
 *   框架读到的内容才不是半成品。
 */
#include "framework/src/fw_internal.h"
#include "platform/port/pf_cap.h"

/** 注册表：下标与 `pf_cap.h` 的位号一一对应，见 `fw_bit_index()`。 */
static const void *s_cap_ops[PF_CAP_COUNT];

/** 位号 → 表下标。0、多位组合、保留位一律 -1（与 `cap_available()` 同一条规矩：
 *  问"这一样有没有"的时候传两位，说明调用方想要的不是一个是非）。 */
static int fw_bit_index(uint32_t bit)
{
    switch (bit) {
    case PF_CAP_DISPLAY:
        return 0;
    case PF_CAP_DISPLAY_PARTIAL:
        return 1;
    case PF_CAP_TOUCH:
        return 2;
    case PF_CAP_KEY:
        return 3;
    case PF_CAP_STORAGE:
        return 4;
    case PF_CAP_NET:
        return 5;
    case PF_CAP_AUDIO_OUT:
        return 6;
    case PF_CAP_AUDIO_IN:
        return 7;
    case PF_CAP_RTC:
        return 8;
    case PF_CAP_UART:
        return 9;
    default:
        return -1;
    }
}

int fw_cap_register(uint32_t bit, const void *ops)
{
    int idx = fw_bit_index(bit);

    if (idx < 0) {
        return -1;
    }
    if (ops == NULL) {
        return -1; /* "注册一个空实现"是错的：要表达"没有"，不注册即可 */
    }
    if (CM_ATOMIC_LOAD(&s_cap_ops[idx], CM_MEM_ACQUIRE) != NULL) {
        return -1; /* 重复注册见头注释：谁生效取决于调用顺序，不接受 */
    }
    CM_ATOMIC_STORE(&s_cap_ops[idx], ops, CM_MEM_RELEASE);
    return 0;
}

const void *fw_cap_ops(uint32_t bit)
{
    int idx = fw_bit_index(bit);

    if (idx < 0) {
        return NULL;
    }
    return CM_ATOMIC_LOAD(&s_cap_ops[idx], CM_MEM_ACQUIRE);
}

bool fw_caps_reported(uint32_t bit)
{
    const pf_ops_t *ops = fw_ops();

    if (ops == NULL) {
        return false;
    }
    if (fw_bit_index(bit) < 0) {
        return false;
    }
    return (ops->caps() & bit) != 0u;
}

void fw_cap_forget(void)
{
    uint32_t i;

    for (i = 0u; i < PF_CAP_COUNT; i++) {
        CM_ATOMIC_STORE(&s_cap_ops[i], (const void *)NULL, CM_MEM_RELEASE);
    }
}
