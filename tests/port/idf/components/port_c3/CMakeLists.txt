# 端口包装：真正的实现住在 `platform/<板>/`（一块板一个目录）。
# 这个组件按 **IDF_TARGET** 选哪一块板，所以同一份自检工程既能跑 C3 也能跑 S3：
#
#   C3（ai-passport）      idf.py set-target esp32c3
#   S3（development board） idf.py set-target esp32s3
#
# 为什么要有包装：host 构建（build.sh）会把 platform/ 下所有 .c 编进去，
# 而 ESP-IDF 的实现 include 了 freertos/ 与 esp_timer.h —— 所以 build.sh
# 排除那两个目录，由本工程单独编译它们。
idf_build_get_property(idf_target IDF_TARGET)

if(idf_target STREQUAL "esp32s3")
    set(EAIS_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/esp32s3-devkit")
else()
    set(EAIS_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/ai-passport-c3")
endif()

message(STATUS "EAIS port for ${idf_target}: ${EAIS_PORT_DIR}")

idf_component_register(
    SRCS "${EAIS_PORT_DIR}/pf_espidf.c"
    INCLUDE_DIRS "${EAIS_PORT_DIR}"
                 "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/port"
    REQUIRES esp_timer
)
