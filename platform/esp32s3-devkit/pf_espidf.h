/**
 * @file pf_espidf.h
 * @brief ESP32-S3 开发板（ESP-IDF）的端口表入口。
 *
 * 这个头只做一件事：把"这张板"与"九项契约"接起来。框架只认
 * `platform/port/pf_ops.h` 里的 `pf_ops_t`，不认任何具体板子。
 *
 * 与 `platform/ai-passport-c3/` 的区别只有三条，都写进 README：
 * 双核、有 PSRAM、日志走 UART 桥而不是 USB-Serial-JTAG。
 */
#ifndef ESP32S3_DEVKIT_PF_ESPIDF_H
#define ESP32S3_DEVKIT_PF_ESPIDF_H

#include "pf_ops.h"   /* `platform/port/` 由构建系统放进 include 路径（与 C3 端口一致） */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 取得本板的端口表。
 *
 * @return 永远非 NULL 的静态表；调用方不释放，也不许改。
 *
 * @note 线程：任意任务可调，也可在初始化阶段调；
 *       中断：不可调（取表不是中断路径上的事）。
 */
const pf_ops_t *pf_platform_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32S3_DEVKIT_PF_ESPIDF_H */
