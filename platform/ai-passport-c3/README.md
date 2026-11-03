# platform/ai-passport-c3/

FoloToy AI Passport（**ESP32-C3** · 8 MB Flash · 无 PSRAM · ESP-IDF 5.5.3）的端口实现。

| 文件 | 是什么 |
|---|---|
| `pf_espidf.c` | 九项端口实现（`esp_timer` / `xSemaphore` / `xTaskCreate`） |
| `pf_espidf.h` | `pf_espidf_ops` 与 `pf_platform_ops()` |

## 这条目录与 host 构建的关系

`build.sh` 会把 `platform/` 下所有 `*.c` 编进 host 测试，而这里的实现 include 了
`freertos/*` 与 `esp_timer.h`，host 上根本编不过。所以 `build.sh` 显式排除本目录：

```sh
find ... -name '*.c' ! -path './platform/ai-passport-c3/*'
```

## 怎么验它

```bash
cd tests/port/idf
source ~/.espressif/tools/activate_idf_v5.5.3.sh
idf.py set-target esp32c3 && idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash          # 需要授权
perl -e 'alarm 15; exec @ARGV' idf.py -p /dev/cu.usbmodemXXXX monitor
```

通过标准：串口上 `SUMMARY pass=14 fail=0 skip=0 test_port_conformance` 与 `DEVICE_DONE rc=0`。
**这套用例在 host 上也跑**（`./build.sh run` 验的是 `platform/host-fake`），同一份源码、两个平台。

## 已知的两处偏离与一条实测结论

- `sleep_ms(0)` → `taskYIELD()`（`vTaskDelay(0)` 不让出 CPU）。
- `sleep_ms(1..9)` 与 `mutex_take` 的超时**向上取整到至少 1 tick**：100 Hz tick 下宁可多睡一个 tick，也不许退化成忙等。
- **实测结论（2026-11-03，真机）**：`APP_STACK_MIN` 原来是 1024，而在一张 1024 字节的栈上只调一次
  `fw_logp()`（4 个参数 + 120 字节字符串）就会 `Guru Meditation Error: ... (Stack protection fault)` 复位；
  同样调用在 2048 字节栈上安全返回。已把 `APP_STACK_MIN` 提到 **3072**（实测下限 2048 + 一档余量），
  并用 `tests/port/idf/main/idf_main.c` 的 `run_probe()` 复验：2048 与 3072 都 `survived`。
  `fw_vlog()` 自己占 256 字节栈缓冲，再叠 newlib 的 `vsnprintf()`——**"能打一行日志"是 App 的最低门槛**，
  这条门槛不能靠 host 上没崩来推。
