/**
 * @file pf_espidf.h
 * @brief FoloToy AI Passport（ESP32-C3 + ESP-IDF 5.5.3）的端口表。
 *
 * 这是"第二块板"的参考实现：它只依赖 `platform/port/pf_ops.h`，不 include
 * 框架与 App 的任何东西——接板的人只需要读 pf_ops.h 和一个这样的目录。
 */
#ifndef PF_ESPIDF_H
#define PF_ESPIDF_H

#include "pf_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 当前平台实现的端口表。成员语义见 `platform/port/pf_ops.h`。 */
extern const pf_ops_t pf_espidf_ops;

/**
 * @brief 取得本平台的端口表（`app_main()` 通过它拿到九项实现）。
 *
 * @return 永远非 NULL，指向 `pf_espidf_ops` 的静态存储。
 *
 * 线程与中断：任意上下文可调用。
 * 注意事项：返回的是静态表，调用方不得修改，也不得释放。
 */
const pf_ops_t *pf_platform_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* PF_ESPIDF_H */
