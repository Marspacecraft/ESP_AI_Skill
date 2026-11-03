# 框架 + 公共库（不含 host-fake：真机上不编它）。
#
# 但**必须排除 framework/src/fw_main.c**：它定义的是整机的 `app_main()`
# 与清单钩子，而本工程有自己的 `app_main`（跑用例的那个）。
file(GLOB SKILL_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../common/src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../framework/src/*.c"
)
list(FILTER SKILL_SRCS EXCLUDE REGEX "/fw_main\\.c$")

idf_component_register(
    SRCS ${SKILL_SRCS}
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../.."
                 "${CMAKE_CURRENT_LIST_DIR}/../../../../../common/include"
                 "${CMAKE_CURRENT_LIST_DIR}/../../../../../framework/include"
                 "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/port"
    REQUIRES esp_timer
)
