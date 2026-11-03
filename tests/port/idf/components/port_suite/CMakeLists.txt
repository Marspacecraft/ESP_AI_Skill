# 同一份契约套件（host 上也跑它）。这里只是把它编进固件，
# `main()` 由 main/idf_main.c 调用。
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../test_port_conformance.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../../test_app_behavior.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../../test_event_on_device.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../.."
                 "${CMAKE_CURRENT_LIST_DIR}/../../../../../tests"
                 "${CMAKE_CURRENT_LIST_DIR}/../../../../../platform/port"
    REQUIRES skill_core port_c3
)
