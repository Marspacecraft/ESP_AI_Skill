/* framework/src/fw_log.c —— 使用面里唯一"不做别的事"的两个函数（步骤 4.1 附带）。
 *
 * 它们存在的理由是诊断：启动失败时如果没有输出，现场就是"上电没动静"。
 * 格式化在这里做，真正落到哪由平台的 `pf_platform_log()` 决定——框架不认识串口。
 *
 * 两条硬规矩：
 *   1. **不分配内存**。日志可能在内存耗尽之后被调用（那正是最需要日志的时候），
 *      所以用栈上的定长缓冲，超出就截断。
 *   2. **不递归**。平台实现里不许再调 `fw_log()`；这里也不调用任何会打日志的框架函数。
 */
#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

#include <stdarg.h>
#include <stdio.h>

/** 一行日志的缓冲上限。真机上串口一行 256 字节已经很奢侈了。 */
#define FW_LOG_BUF 256u

static void fw_vlog(const char *tag, const char *fmt, va_list ap)
{
    char buf[FW_LOG_BUF];
    int n = 0;

    if (fmt == NULL) {
        return;
    }
    if (tag != NULL) {
        n = snprintf(buf, sizeof buf, "[%s] ", tag);
        if (n < 0 || (size_t)n >= sizeof buf) {
            return; /* 标签本身就超长：丢弃而不是截断到一半的括号 */
        }
    }
    (void)vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    pf_platform_log(buf);
}

void fw_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fw_vlog(NULL, fmt, ap);
    va_end(ap);
}

void fw_logp(const char *tag, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fw_vlog(tag, fmt, ap);
    va_end(ap);
}
