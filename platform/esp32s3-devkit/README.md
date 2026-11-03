# ESP32-S3 开发板（ESP-IDF 5.5.3）

第二块真板。存在的意义不是"多支持一块硬件"，而是给"换一块板只写本目录、
不动 `framework/`"这条断言（验收线 A8）提供**第二份证据**——第一份是
`platform/ai-passport-c3/`。

## 实测规格

| 项 | 值 |
|---|---|
| 芯片 | ESP32-S3 (QFN56) rev v0.2，**双核** 240 MHz |
| 片内 PSRAM | 8 MB（AP_3v3） |
| Flash | 16 MB（quad） |
| 串口 | FT232R USB-UART 桥（`/dev/cu.usbserial-*`），烧录与日志同一条线 |
| 控制台 | **UART0**（不是 USB-Serial-JTAG） |

## 与 `platform/ai-passport-c3/` 的三处差别

1. **双核**。`xTaskCreate()` 不绑核（框架不提供"绑核"参数，也不需要）：
   跨任务状态只能靠 `__atomic_*` 保证可见性。`volatile` 在单核 C3 上还能
   蒙对，在 S3 上不能——这是 `framework/src/fw_internal.h` 里 D20 的
   访问器在这块板上第一次被真正检验。
2. **`taskYIELD()` 只让当前核**。伪阻塞每片之前的取消位检查依赖的是
   `vTaskDelay()` 真的让出该核，不是 `taskYIELD()`。
3. **控制台走 UART 桥**。`printf` 到 UART0，波特率 115200；抓日志用
   `idf.py monitor`（连上会复位一次）。

## 九项语义

与 C3 完全一致，逐条照 `platform/port/pf_ops.h` 的注释落：`mem_alloc`
只认 `bytes>0`、`mem_free(NULL)` 无操作、`clock_ms` 单调（`esp_timer`
截断成 `uint32_t`，约 49.7 天回绕，用差值比较）、`sleep_ms(0)` 必须让出
一次（`vTaskDelay(0)` 不让出，用 `taskYIELD()`）、`sleep_ms(n>0)` 的 tick
数**向上取整到至少 1**（100 Hz 下 1~9 ms 会截成 0 → 忙等）、互斥非递归、
`task_create` 收字节数、`task_exit_current` 不返回。

`caps()` 只返回 `PF_CAP_UART`：板上有 Wi-Fi/BLE/PSRAM，但本仓库**还没有那些能力头**，
按 architecture §7.3 的裁决，没有可调接口就不许点亮能力位。

## 能力面：串口（第一项真实能力，D58）

`pf_platform_boot()` 在 `fw_init()` 之前被框架调一次，注册串口 vtable；`esp_port_caps()`
同时点亮 `PF_CAP_UART`（位 9）。

| 项 | 值 |
|---|---|
| 控制器 | `UART_NUM_1`（UART0 是控制台/烧录口） |
| TX | **GPIO17**（板级宏 `APP_UART_TX_GPIO`） |
| RX | **GPIO18**（板级宏 `APP_UART_RX_GPIO`） |

**这条能力在 S3 上先红过**：`uart_config_t` 忘了清零 → `E uart: uart_param_config(995):
rx flow thresh error`（`rx_flow_ctrl_thresh` 是栈垃圾）。C3 同一份代码一路绿——
两块板都要烧，才能说"这个文件是对的"。

## 为什么不进 host 构建

`build.sh` 把 `platform/` 下所有 `.c` 编进 host 测试，而本文件 include 了
`freertos/` 与 `esp_timer.h`。所以 `build.sh` 显式排除本目录，由
`tests/port/idf/`（同一个自检工程，`idf.py set-target esp32s3`）单独编译。
