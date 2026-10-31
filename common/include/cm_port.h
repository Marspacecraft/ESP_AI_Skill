#ifndef COMMON_INCLUDE_CM_PORT_H
#define COMMON_INCLUDE_CM_PORT_H

/*
 * cm_port.h —— 标准 C 兼容层：工具链差异的唯一收敛点。
 *
 * 为什么需要它：同一段代码要能在 ESP32（xtensa/riscv GCC）与 host（clang/GCC）
 * 上编译，而属性、对齐关键字、静态断言的写法在各工具链之间并不一致。上层
 * 只写标准 C 与这里的宏，换工具链时只改这一个文件。
 *
 * 本头不含任何函数、不分配内存、不需要初始化。
 */

/* C11 版本闸门：仓库规范是标准 C11（docs/code-structure.md §2.2）。
 * 用 #error 而不是静默降级——C99 下 _Static_assert 不存在，错误会推迟到
 * 某个不相干的编译单元里才炸。 */
#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 201112L)
#error "common/ 要求标准 C11，请使用 -std=c11 编译"
#endif

/* 编译器闸门：cm_atomic.h 依赖 GCC/Clang 的 __atomic_* 内建，本文件依赖其
 * 属性语法。不做"假装支持"的降级——那会让原子操作在别的工具链上退化成
 * 普通读写，而普通读写恰好是最难发现的竞态。 */
#if !defined(__GNUC__) && !defined(__clang__)
#error "common/ 依赖 GCC/Clang 扩展（__atomic_* 与属性宏），当前编译器不受支持"
#endif

/* 头文件里的函数：多个编译单元各有一份副本，不产生外部符号，也不会重复定义 */
#define CM_INLINE static inline

/* 标准关键字的别名：上层不必记拼写，换工具链也不用改代码 */
#define CM_RESTRICT restrict
#define CM_ALIGNOF(type) _Alignof(type)
#define CM_ALIGNAS(n) _Alignas(n)

/* 编译期断言：假设"int 是 4 字节"这类错误在编译期就挡住，不要留到运行期 */
#define CM_STATIC_ASSERT(condition, message) _Static_assert(condition, message)

/* 属性宏：GCC/Clang 语法已在文件头闸门里保证 */
#define CM_NORETURN __attribute__((noreturn))
#define CM_PACKED __attribute__((packed))
#define CM_WEAK __attribute__((weak))
#define CM_DEPRECATED(message) __attribute__((deprecated(message)))
#define CM_PRINTF_FMT(fmt_index, arg_index) \
    __attribute__((format(printf, fmt_index, arg_index)))

/* 分支预测提示：只给热路径的判空/判错用，不要到处撒 */
#define CM_LIKELY(x) __builtin_expect(!!(x), 1)
#define CM_UNLIKELY(x) __builtin_expect(!!(x), 0)

#endif /* COMMON_INCLUDE_CM_PORT_H */
