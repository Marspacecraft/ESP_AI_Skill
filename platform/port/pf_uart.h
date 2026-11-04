/* platform/port/pf_uart.h —— 串口能力契约（平台实现，框架调用）。
 *
 * 这是**第一项真实能力**，也是"能力怎么落地"这条路径的第一个样本：
 *
 *     板子实现本文件的 vtable → 在 pf_platform_boot() 里 fw_cap_register(PF_CAP_UART, &ops)
 *     → fw_ops_check() 校验平台表 → cap_available(PF_CAP_UART) 为真 → App 调 api/af_api/uart.h
 *
 * 它不在 `pf_ops_t` 里，理由见 `platform/port/pf_cap.h` 的头注释：九项是"没有它框架
 * 就跑不起来"的机制，串口不是。
 *
 * 三条使用规则：
 *   1. **逻辑端口号**：`port` 是 0..count-1 的逻辑编号，具体是哪路物理 UART、用哪两根
 *      引脚，由板子在实现里决定并写进它自己的 README。App 只认逻辑号——换板不改 App。
 *   2. **返回字节数，不返回错误码**：`write`/`read` 成功时返回实际字节数（可能少于请求），
 *      失败返回 -1。`cm_err_t` 是框架的词汇，平台层不产出（D33）。
 *   3. **`timeout_ms` 是上界**：0 表示"立刻返回，不等待"。实现可以提前返回，
 *      但**不得**超过这个上界太久（框架按 20 ms 切片调用）。
 *
 * 线程与中断：全部只能从任务上下文调用（框架保证：App 任务或框架自己的初始化）；
 * 不得在 ISR 里调用。本接口不做 DMA、不做中断接收，v1 只有轮询。
 */
#ifndef ESP_AI_SKILL_PF_UART_H
#define ESP_AI_SKILL_PF_UART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PF_UART_OPS_VERSION 1u
#define PF_UART_MAX_PORTS 2u /* 逻辑端口上限；v1 实际只用 1 路 */

/** 串口实现表。板子在 `pf_platform_boot()` 里把它的地址交给
 *  `fw_cap_register(PF_CAP_UART, &ops)`。 */
typedef struct {
    uint32_t version; /* 必须等于 PF_UART_OPS_VERSION；改布局就 +1，旧表不兼容 */
    uint32_t count;   /* 这台板提供几路逻辑端口，取值 1..PF_UART_MAX_PORTS */

    /** 打开一路串口，固定 8N1。
     * @param port 逻辑端口号，0..count-1。
     * @param baud 波特率，例如 115200。
     * @return 0 成功；-1 失败（端口不存在、被占用、波特率不支持）。
     *         线程与中断：任务上下文；调用方保证不会重复打开同一路（框架层不做引用计数）。
     *         注意事项：实现应当幂等失败——重复 open 同一路返回 -1，而不是悄悄成功两次。 */
    int (*open)(uint32_t port, uint32_t baud);

    /** 关闭一路串口。
     * @param port 逻辑端口号，0..count-1。
     * @return 0 成功；-1 失败（端口不存在，或本来就没打开）。
     *         线程与中断：任务上下文。
     *         注意事项：**必须幂等失败**——重复 close 返回 -1，不许崩溃或悄悄成功；
     *                   关掉之后不该再有字节写出去。 */
    int (*close)(uint32_t port);

    /** 写一段字节。
     * @param buf 数据；`len` 可以大于 0，实现不要求一次写完。
     * @param len 请求字节数。
     * @param timeout_ms 本次调用的上界；0 = 只尝试一次，不等待。
     * @return 实际写出的字节数（0..len），或 -1。
     *         线程与中断：任务上下文。
     *         注意事项：**不保证分帧**——框架会把一次 App 写切成若干片，
     *                   对端必须自己按协议组帧，不能假设"一次 write 一个包"。 */
    int (*write)(uint32_t port, const void *buf, size_t len, uint32_t timeout_ms);

    /** 读一段字节。
     * @param cap 缓冲区容量。
     * @param timeout_ms 最多等多久；0 = 立刻返回当前可读内容（可能 0 字节）。
     * @return 读到的字节数（0 表示超时内没有数据，这**不是错误**），或 -1 失败。
     *         线程与中断：任务上下文。
     *         注意事项：0 与 -1 必须分开——"对端没说话"和"串口坏了"是两件事。 */
    int (*read)(uint32_t port, void *buf, size_t cap, uint32_t timeout_ms);
} pf_uart_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_PF_UART_H */
