#ifndef COMMON_INCLUDE_CM_TYPES_H
#define COMMON_INCLUDE_CM_TYPES_H

/*
 * cm_types.h —— 全项目共用的基础类型别名与通用宏。
 *
 * 为什么需要它：<stdint.h> 已经保证定宽，但各模块会各挑一套写法
 * （uint32_t / unsigned int / u32），跨层传参时同一个宽度出现三种名字。
 * 这里统一给出 cm_ 别名与四个反复用到的宏，除此之外不新增任何抽象。
 *
 * 本头只含类型与宏：不含函数、不分配内存、不需要初始化。
 */

#include "cm_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 定宽别名：与 <stdint.h> 一一对应，宽度的保证见文件末尾的静态断言。
 * 不加 _t 后缀的理由：它们不是句柄（handle），只是标准整数的短名字；
 * 与 uint8_t 语义完全相同，写 cm_u8 只为在 MCU 代码里短一截。 */
typedef uint8_t cm_u8;    /* 无符号 8 位，范围 0..255 */
typedef uint16_t cm_u16;  /* 无符号 16 位，范围 0..65535 */
typedef uint32_t cm_u32;  /* 无符号 32 位；毫秒时间戳与环形缓冲区索引都用它 */
typedef uint64_t cm_u64;  /* 无符号 64 位；只在 host 侧统计与日志里用 */
typedef int8_t cm_i8;     /* 有符号 8 位 */
typedef int16_t cm_i16;   /* 有符号 16 位；采样值常用 */
typedef int32_t cm_i32;   /* 有符号 32 位；相对时间差与回绕比较用它 */
typedef int64_t cm_i64;   /* 有符号 64 位 */

/* 数组元素个数。
 * 只对真数组有效：传指针进来会静默算出错误结果（指针的 sizeof 是指针宽度），
 * 所以调用点必须确认实参是数组而不是退化后的指针。 */
#define CM_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/* 取小 / 取大。
 * 注意：两个参数都会被求值两次，不要传 i++ 这类带副作用的表达式；
 *       类型不同时按 C 的通常算术转换比较，混用有符号/无符号要先自己转。 */
#define CM_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define CM_MAX(a, b) (((a) > (b)) ? (a) : (b))

/* 由成员地址反推宿主结构体地址（侵入式容器的唯一取回手段）。
 * 入参：ptr    成员指针，必须非 NULL 且确实指向 type 的 member 字段
 *       type   宿主结构体类型名
 *       member 成员名
 * 返回：宿主结构体指针；ptr 为 NULL 时结果无意义（本宏不做检查）
 * 线程：纯宏，无状态
 * 注意：ptr 与 member 不匹配时行为未定义，编译期查不出来 */
#define CM_CONTAINER_OF(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* 向上 / 向下对齐。
 * 注意：a 必须是 2 的幂（它是运行时值，静态断言查不到），否则结果是错的；
 *       位运算版本比取模快，代价就是这个前提。 */
#define CM_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define CM_ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* 宽度的编译期保证：这些别名是跨层传参的类型基础，宽度错了要在编译期红，
 * 而不是等某个协议解析在板上多读一个字节。 */
CM_STATIC_ASSERT(sizeof(cm_u8) == 1, "cm_u8 必须是 1 字节");
CM_STATIC_ASSERT(sizeof(cm_u16) == 2, "cm_u16 必须是 2 字节");
CM_STATIC_ASSERT(sizeof(cm_u32) == 4, "cm_u32 必须是 4 字节");
CM_STATIC_ASSERT(sizeof(cm_u64) == 8, "cm_u64 必须是 8 字节");
CM_STATIC_ASSERT(sizeof(cm_i8) == 1, "cm_i8 必须是 1 字节");
CM_STATIC_ASSERT(sizeof(cm_i16) == 2, "cm_i16 必须是 2 字节");
CM_STATIC_ASSERT(sizeof(cm_i32) == 4, "cm_i32 必须是 4 字节");
CM_STATIC_ASSERT(sizeof(cm_i64) == 8, "cm_i64 必须是 8 字节");

#ifdef __cplusplus
}
#endif

#endif /* COMMON_INCLUDE_CM_TYPES_H */
