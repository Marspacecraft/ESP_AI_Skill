/* framework/src/fw_uart.c —— 串口能力的 App 门面（实现）。
 *
 * 这一层负责四件事，**一件都不下放给平台**：
 *
 *   1. 判"这项能力在不在"：硬件位与注册表都为真才放行（`cap_available()` 同一条规矩）；
 *   2. 把平台的 `0/-1` 翻译成 `cm_err_t`（平台层不产出错误码，D33）；
 *   3. 切片：写按 32 字节、读按 20 ms，**片与片之间调用 `fw_cancel_point()`**——
 *      取消位命中就走退出路径、不返回。这是"伪阻塞"在能力层的落点；
 *   4. 记账：哪一路已经打开（板子只说成败，重复 open 由这里判 `CM_ERR_BUSY`）。
 *
 * 为什么记账放在框架而不是平台：`CM_ERR_BUSY`（已经开了）和 `CM_ERR_UNAVAILABLE`
 * （硬件打不开）是两种处置——前者是调用方写错了，后者是现场问题。让平台用一个
 * `-1` 同时表达两件事，调用方就只能猜。
 */
#include "api/af_api/cap.h"
#include "api/af_api/uart.h"
#include "framework/src/fw_internal.h"
#include "platform/port/pf_uart.h"

/* 一次交给平台的最大字节数。取 32 是为了让"取消"在慢速串口（9600 bps ≈ 1.2 KB/s）
 * 上也能在 20~30 ms 内被响应，而不是等一整块发完。 */
#define FW_UART_WRITE_CHUNK 32u

/* 读的切片长度：到点就回到框架查一次取消位。 */
#define FW_UART_READ_SLICE_MS 20u

/* 平台连续这么久没有任何进展，就判定超时（不是"再来一次"，见 uart_write 注释）。 */
#define FW_UART_NO_PROGRESS_MS 500u

/** 哪一路已经打开。板子只有 PF_UART_MAX_PORTS 路，一个位图够用。 */
static uint32_t s_opened;

/** 取串口实现表；没有这项能力（未注册 / 硬件没报）返回 NULL。
 *  版本不符也按"没有"处理——表布局变了，硬解引用是在读未定义内存。 */
static const pf_uart_ops_t *uart_ops(void)
{
    const pf_uart_ops_t *ops;

    if (!cap_available(PF_CAP_UART)) {
        return NULL;
    }
    ops = (const pf_uart_ops_t *)fw_cap_ops(PF_CAP_UART);
    if (ops == NULL || ops->version != PF_UART_OPS_VERSION) {
        return NULL;
    }
    if (ops->count == 0u || ops->count > PF_UART_MAX_PORTS) {
        return NULL;
    }
    return ops;
}

static bool port_valid(const pf_uart_ops_t *ops, uint32_t port)
{
    return (ops != NULL) && (port < ops->count);
}

static bool opened(uint32_t port)
{
    return (s_opened & (1u << port)) != 0u;
}

static void set_opened(uint32_t port, bool on)
{
    if (on) {
        s_opened |= (1u << port);
    } else {
        s_opened &= ~(1u << port);
    }
}

void fw_uart_forget(void)
{
    s_opened = 0u;
}

uint32_t uart_port_count(void)
{
    const pf_uart_ops_t *ops = uart_ops();

    return (ops != NULL) ? ops->count : 0u;
}

cm_err_t uart_open(uint32_t port, uint32_t baud)
{
    const pf_uart_ops_t *ops = uart_ops();

    if (ops == NULL) {
        return CM_ERR_NOT_SUPPORTED;
    }
    if (!port_valid(ops, port) || baud == 0u) {
        return CM_ERR_INVALID;
    }
    if (opened(port)) {
        return CM_ERR_BUSY; /* 没有引用计数：重复 open 就是调用方多开了一次 */
    }
    if (ops->open == NULL || ops->open(port, baud) != 0) {
        return CM_ERR_UNAVAILABLE;
    }
    set_opened(port, true);
    return CM_OK;
}

cm_err_t uart_close(uint32_t port)
{
    const pf_uart_ops_t *ops = uart_ops();

    if (ops == NULL) {
        return CM_ERR_NOT_SUPPORTED;
    }
    if (!port_valid(ops, port)) {
        return CM_ERR_INVALID;
    }
    if (!opened(port)) {
        return CM_ERR_STATE;
    }
    if (ops->close == NULL || ops->close(port) != 0) {
        return CM_ERR_UNAVAILABLE;
    }
    set_opened(port, false);
    return CM_OK;
}

cm_err_t uart_write(uint32_t port, const void *data, size_t len, size_t *out_written)
{
    const pf_uart_ops_t *ops = uart_ops();
    const unsigned char *p = (const unsigned char *)data;
    size_t done = 0u;
    uint32_t last_progress = 0u;

    if (out_written != NULL) {
        *out_written = 0u;
    }
    if (ops == NULL) {
        return CM_ERR_NOT_SUPPORTED;
    }
    if (data == NULL || !port_valid(ops, port)) {
        return CM_ERR_INVALID;
    }
    if (fw_current_ctx() == NULL) {
        return CM_ERR_STATE; /* 伪阻塞点必须能被打断，所以必须在 App 任务里 */
    }
    if (!opened(port)) {
        return CM_ERR_STATE;
    }
    if (len == 0u) {
        return CM_OK;
    }

    last_progress = fw_ops()->clock_ms();
    while (done < len) {
        size_t chunk = len - done;
        int n;

        fw_cancel_point(); /* 片间取消点：命中即走退出，不返回 */

        if (chunk > FW_UART_WRITE_CHUNK) {
            chunk = FW_UART_WRITE_CHUNK;
        }
        n = (ops->write != NULL) ? ops->write(port, p + done, chunk, FW_UART_NO_PROGRESS_MS / 4u)
                                 : -1;
        if (n < 0) {
            if (out_written != NULL) {
                *out_written = done;
            }
            return CM_ERR_UNAVAILABLE;
        }
        if (n > 0) {
            done += (size_t)n;
            last_progress = fw_ops()->clock_ms();
            continue;
        }
        /* 平台一点也没写出去：可能是发送缓冲满了。不算错，继续等到有进展为止，
         * 但"一直没有进展"是有上界的——否则这就是一个不可取消的忙等。 */
        if ((uint32_t)(fw_ops()->clock_ms() - last_progress) >= FW_UART_NO_PROGRESS_MS) {
            if (out_written != NULL) {
                *out_written = done;
            }
            return CM_ERR_TIMEOUT;
        }
        fw_delay_block(1u); /* 让出一次，顺便再查一次取消位 */
    }

    if (out_written != NULL) {
        *out_written = done;
    }
    return CM_OK;
}

cm_err_t uart_read(uint32_t port, void *buf, size_t cap, size_t *out_len, uint32_t timeout_ms)
{
    const pf_uart_ops_t *ops = uart_ops();
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0u;
    uint32_t start;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (ops == NULL) {
        return CM_ERR_NOT_SUPPORTED;
    }
    if (buf == NULL || cap == 0u || !port_valid(ops, port)) {
        return CM_ERR_INVALID;
    }
    if (fw_current_ctx() == NULL) {
        return CM_ERR_STATE;
    }
    if (!opened(port)) {
        return CM_ERR_STATE;
    }

    start = fw_ops()->clock_ms();
    for (;;) {
        uint32_t slice = FW_UART_READ_SLICE_MS;
        uint32_t elapsed = (uint32_t)(fw_ops()->clock_ms() - start);
        int n;

        fw_cancel_point(); /* 切片间取消点 */

        if (timeout_ms > elapsed && (timeout_ms - elapsed) < slice) {
            slice = timeout_ms - elapsed;
        }
        n = (ops->read != NULL) ? ops->read(port, p + got, cap - got, slice) : -1;
        if (n < 0) {
            if (out_len != NULL) {
                *out_len = got;
            }
            return CM_ERR_UNAVAILABLE;
        }
        if (n > 0) {
            got += (size_t)n;
            if (out_len != NULL) {
                *out_len = got;
            }
            return CM_OK; /* 读到就返回，不等满超时——"读一段"不是"读满期待的长度" */
        }
        if (timeout_ms == 0u || (uint32_t)(fw_ops()->clock_ms() - start) >= timeout_ms) {
            if (out_len != NULL) {
                *out_len = got;
            }
            return CM_ERR_TIMEOUT; /* 等满了还没数据：对端没说话，不是串口坏了 */
        }
        fw_delay_block(1u);
    }
}
