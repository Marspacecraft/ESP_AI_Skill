# 独立 ESP-IDF 工程：把端口契约套件编成固件，在真机上跑一遍。
#
# 它**不参与** ai-passport 的构建，也不链它的 BSP —— 目的只是回答一个问题：
# `pf_ops.h` 里写的九条语义，在 ESP32-C3 上是否成立。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp_ai_skill_port)
