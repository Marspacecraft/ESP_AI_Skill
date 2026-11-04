/* api/af_api/uart.h —— 串口（App 可见的契约）。
 *
 * 这是**第一项真实能力**，也是后面所有能力头的模板。它同时演示了能力的三个层次：
 *
 *   1. 硬件在不在：`cap_available(CAP_UART)`（板子报的位）；
 *   2. 软件能不能调：板子通过 `fw_cap_register()` 交了 `pf_uart_ops_t`；
 *   3. 怎么用：本文件的四个函数。
 *
 * 只有 (1) 没有 (2) 时，`cap_available()` 回答 false、这里的函数返回
 * `CM_ERR_NOT_SUPPORTED`——"这块板没这个能力"是**正常路径**，不是错误。
 *
 * 依赖约束：只能 include `common/`（与 `app.h`/`res.h`/`evt.h`/`svc.h`/`cap.h` 相同）。
 * 平台表 `pf_ops_t` 与 `pf_uart_ops_t` 对 App **不可见**。
 *
 * 线程与中断：`uart_open`/`uart_close` 任意任务可调；`uart_write`/`uart_read`
 * **只能在 App 任务里**调（它们靠取消位实现伪阻塞，退出时必须能被打断）。
 * ISR 里一个都不许调——串口输出在中断里会长时间占用。
 */
#ifndef ESP_AI_SKILL_AF_API_UART_H
#define ESP_AI_SKILL_AF_API_UART_H

#include <stddef.h>
#include <stdint.h>

#include "common/include/cm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 逻辑端口号。板子把它映射到某一路物理 UART；换板不改 App。
 *  v1 每块板只有这一路；`uart_port_count()` 返回实际路数。 */
#define UART_PORT_APP 0u

/** 这块板提供几路逻辑串口（0 表示没有，或没有实现）。
 *
 * @return 路数；未注册实现时返回 0。
 *         线程与中断：任意任务；不阻塞、不分配。
 *         注意事项：0 有两个来源——板子没这能力，或能力实现了但没注册
 *                   （板级 `pf_platform_boot()` 漏了）。要分清只能看 `cap_available()`。
 */
uint32_t uart_port_count(void);

/** 打开一路串口（固定 8N1）。
 *
 * @param port 逻辑端口号，`0 .. uart_port_count()-1`；越界 → `CM_ERR_INVALID`。
 * @param baud 波特率，常见 9600/115200/921600；实现不支持 → `CM_ERR_INVALID`。
 * @return `CM_OK`；`CM_ERR_NOT_SUPPORTED` 没有这项能力（未注册或被平台判为没有）；
 *         `CM_ERR_INVALID` 参数越界；`CM_ERR_BUSY` 这一路已经打开。
 *         线程与中断：任意任务；不阻塞。
 *         注意事项：没有引用计数——同一路重复 open 返回 `CM_ERR_BUSY`，
 *                   必须配对 `uart_close()`。
 */
cm_err_t uart_open(uint32_t port, uint32_t baud);

/** 关闭一路串口。
 *
 * @param port 逻辑端口号；越界 → `CM_ERR_INVALID`。
 * @return `CM_OK`；`CM_ERR_NOT_SUPPORTED` 没有这项能力；
 *         `CM_ERR_INVALID` 端口越界；`CM_ERR_STATE` 这一路没打开。
 *         线程与中断：任意任务；不阻塞。
 *         注意事项：与 `uart_open()` 配对；没有引用计数，关一次就真的关了。
 */
cm_err_t uart_close(uint32_t port);

/** 把一段字节全部写出去（**伪阻塞点**）。
 *
 * 框架把它切成若干片（每片 ≤32 字节）依次交给平台，**片与片之间检查取消位**：
 * 命中取消就走退出路径，本函数不返回。所以本函数之后的代码不一定会执行。
 *
 * @param port 逻辑端口号。
 * @param data 数据；为 NULL → `CM_ERR_INVALID`。
 * @param len  字节数；`0` → `CM_OK`（什么都没写，不算错误）。
 * @param out_written 可选出参：实际写出的字节数。出错时也可能非 0（部分已写出）。
 *                    可为 NULL。
 * @return `CM_OK` 全部写完；`CM_ERR_NOT_SUPPORTED` 没有这项能力；
 *         `CM_ERR_INVALID` 参数非法；`CM_ERR_STATE` 不在 App 任务里 / 这一路没打开；
 *         `CM_ERR_TIMEOUT` 平台连续 500 ms 没有任何进展。
 *         线程与中断：**只能在 App 任务里**；ISR 禁止。
 *         注意事项：**不保证分帧**——对端必须自己按协议组帧，不能假设
 *                   "一次 `uart_write` 正好一个包"。
 */
cm_err_t uart_write(uint32_t port, const void *data, size_t len, size_t *out_written);

/** 读一段字节（**伪阻塞点**）。
 *
 * 按 20 ms 切片轮询平台，切片之间检查取消位（命中即走退出路径，不返回）。
 *
 * @param port 逻辑端口号。
 * @param buf  接收缓冲；为 NULL 或 `cap == 0` → `CM_ERR_INVALID`。
 * @param cap  缓冲区容量。
 * @param out_len 出参：实际读到的字节数（成功时必写）。
 * @param timeout_ms 总等待上界；`0` = 只取当前可读内容，立刻返回。
 * @return `CM_OK` 读到了至少 1 字节；`CM_ERR_TIMEOUT` 等满 `timeout_ms` 仍没有数据
 *         （**这不是错误，是对端没说话**）；`CM_ERR_NOT_SUPPORTED` 没有这项能力；
 *         `CM_ERR_INVALID` 参数非法；`CM_ERR_STATE` 不在 App 任务里 / 没打开。
 *         线程与中断：**只能在 App 任务里**；ISR 禁止。
 *         注意事项：这是伪阻塞点——等待期间命中取消位会走退出路径且**不返回**，
 *                   所以 `uart_read()` 之后的代码不一定会执行。
 */
cm_err_t uart_read(uint32_t port, void *buf, size_t cap, size_t *out_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AI_SKILL_AF_API_UART_H */
