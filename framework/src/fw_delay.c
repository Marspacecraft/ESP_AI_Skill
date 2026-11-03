/* framework/src/fw_delay.c —— `fw_delay_block()`：伪阻塞的落点。
 *
 * App 写 `for (;;) { do_work(); fw_delay_block(100); }`，它不知道自己会被取消；
 * 取消是在**这一次睡眠内部**发生的：把 100ms 切成 20ms 一片，每片之前查一次
 * 取消位，命中就走退出路径——而退出路径不返回。所以 `fw_delay_block()` 之后
 * 那行代码"可能永远不执行"，这不是异常，是正常路径。
 *
 * 两个容易写错的地方：
 *   1. 查取消位要在**每片之前**，不是整段之前。整段只查一次等于把响应延迟
 *      从 20ms 放大到最长 100ms（App 自己写的数可能是几百毫秒）。
 *   2. 最后一片睡完之后还要再查一次。取消可能正好落在最后一片里，漏掉这一查
 *      就要等下一次 `fw_delay_block()` 才响应——"停不下来"的观感就是这么来的。
 *
 * `in_exit` 守卫：`exit()` 内部允许再调阻塞接口（清理时常常要等外设）。
 * 那条路径上取消位已经被清掉；如果这期间又来了新请求，再走一次退出就是
 * 在清理过程里二次结束任务——所以退出期间取消位失效。
 */
#include "framework/src/fw_internal.h"

void fw_cancel_point(void)
{
    struct fw_app_ctx *c = fw_current_ctx();

    if (c == NULL) {
        return; /* 不在 App 任务里：没有取消位可查 */
    }
    if (CM_ATOMIC_LOAD(&c->in_exit, CM_MEM_ACQUIRE) != 0u) {
        return; /* 已经在 exit() 里：取消位失效，否则会在清理中途二次结束任务 */
    }
    if (CM_ATOMIC_LOAD(&c->cancel, CM_MEM_ACQUIRE) != 0u) {
        fw_run_exit(c, FW_APP_STATE_STOPPED); /* 不返回 */
    }
}

void fw_delay_block(uint32_t ms)
{
    const pf_ops_t *ops = fw_ops();
    struct fw_app_ctx *c;
    uint32_t remaining = ms;

    if (ops == NULL || ops->sleep_ms == NULL) {
        return; /* 没绑平台表：无处可睡，也无可等待 */
    }

    c = fw_current_ctx();

    fw_heartbeat_touch(c);
    for (;;) {
        /* 每片之前查一次；最后一片之后回到这里，所以"最后一片里被取消"也能抓到。 */
        fw_cancel_point();
        if (remaining == 0u) {
            break;
        }
        {
            uint32_t slice = (remaining > FW_DELAY_SLICE_MS) ? FW_DELAY_SLICE_MS : remaining;
            ops->sleep_ms(slice);
            remaining -= slice;
        }
    }

    /* `ms==0` 的语义是"让出一次 CPU"，不推进时间：上面的循环只做了一次
     * 取消位检查（该做的都做了），这里补上真正的让出。 */
    if (ms == 0u) {
        ops->sleep_ms(0u);
    }
}
