/* common/src/cm_types.c —— 错误码短名（只用于日志与测试输出）。 */
#include "common/include/cm_types.h"

const char *cm_err_str(cm_err_t e)
{
    switch (e) {
    case CM_OK:                return "CM_OK";
    case CM_ERR_INVALID:       return "CM_ERR_INVALID";
    case CM_ERR_NOMEM:         return "CM_ERR_NOMEM";
    case CM_ERR_NOT_SUPPORTED: return "CM_ERR_NOT_SUPPORTED";
    case CM_ERR_UNAVAILABLE:   return "CM_ERR_UNAVAILABLE";
    case CM_ERR_VERSION:       return "CM_ERR_VERSION";
    case CM_ERR_TIMEOUT:       return "CM_ERR_TIMEOUT";
    case CM_ERR_BUSY:          return "CM_ERR_BUSY";
    case CM_ERR_STATE:         return "CM_ERR_STATE";
    case CM_ERR_CANCELED:      return "CM_ERR_CANCELED";
    case CM_ERR_QUOTA:         return "CM_ERR_QUOTA";
    }
    return "CM_ERR_?";
}
