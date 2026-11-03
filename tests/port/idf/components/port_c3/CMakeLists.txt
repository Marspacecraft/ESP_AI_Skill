# 这个组件只是个"包装"：真正的端口实现住在 platform/ai-passport-c3/，
# 那是它的正式位置（一块板一个目录）。这里只负责把它编进这个自检工程。
#
# 为什么要有包装：host 构建（build.sh）会把 platform/ 下所有 .c 编进去，
# 而 ESP-IDF 的实现 include 了 freertos/ 与 esp_timer.h —— 所以 build.sh
# 排除这个目录，由本工程单独编译它。
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/ai-passport-c3/pf_espidf.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/ai-passport-c3"
                 "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/port"
    REQUIRES esp_timer
)
