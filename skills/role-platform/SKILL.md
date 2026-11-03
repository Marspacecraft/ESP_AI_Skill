---
name: role-platform
description: 在 ESP AI Skill 仓库里把一块新板接进来、实现或修改 pf_ops_t 九项、跑 tests/port 契约套件、烧录真机并抓串口时使用——用户说"接这块板""端口实现""pf_ops""真机跑一下""Guru Meditation""看门狗复位""单核/双核""烧录""monitor"，或你要动 platform/<板>/ 时先读这份。
---

# 角色 · 平台层（tag 4）

你实现的是**九项原语**，不是"一块板的功能"。框架只认 `pf_ops.h` 里那九个函数指针：
内存、时钟、睡眠、互斥、任务。屏幕、按键、音频、存储、网络**都不在这一层**——它们将来是
服务 App + 能力位的事。

全局规矩（交清单、四行交付、停手）见 [`AGENTS.md`](../../AGENTS.md)。这里只写这一层独有的判断。

---

## 1. 动手前先确认你写得了

```bash
python3 tools/check_workspace_policy.py --as 4 platform/esp32s3-devkit/pf_espidf.c
# ALLOW  platform/esp32s3-devkit/pf_espidf.c  role 4：platform/esp32s3-devkit/   （退出码 0）

python3 tools/check_workspace_policy.py --as 4 framework/src/fw_ctx.c
# DENY   framework/src/fw_ctx.c 属于 role 3；--as 4 不在其中                     （退出码 1 → 停）
```

你的写区：`platform/<你那块板>/`、`tests/platform/`、`docs/platform/`、`framework/include/`（共写）。

**要人批准的两处**（都是 `nobody`，你改不了）：

1. `policy.txt` 的 `role 4` 加一行 `platform/<新板>/`——否则你的目录"不属于任何人"。
2. `build.sh` 的库源 find 加 `! -path 'platform/<新板>/*'`——端口要 include `freertos/`、`esp_timer.h`，
   host 编译过不去。**注意 `find` 返回的路径不带 `./` 前缀**，写成 `./platform/...` 会永远匹配不上
   （踩过：排除失效、15 个 host 测试全红）。

---

## 2. 九项的语义红线

| 项 | 必须 | 不许 |
|---|---|---|
| `mem_alloc` | `bytes==0` 返回 NULL；NULL 是唯一失败信号 | 偷偷抬大、返回对齐陷阱地址 |
| `mem_free` | `NULL` 无操作 | 对非本分配器的指针动手 |
| `clock_ms` | 单调、约 49.7 天回绕（`uint32_t`） | 用差值以外的比较；返回本地时间 |
| `sleep_ms` | **真让出 CPU**；`ms==0` 也让出一次 | 忙等（会让伪阻塞整套失效） |
| `mutex_create` | 成功返回句柄 | 返回 NULL（框架自己要锁，NULL 视为平台违约） |
| `mutex_take` | **相对超时**；`timeout==0` 只试一次；0=拿到 / -1=没拿到 | 绝对超时；`m==NULL` 时崩溃 |
| `mutex_give` | `NULL` 无操作 | 非持有者也能解锁 |
| `task_create` | `stack_bytes < PF_TASK_MIN_STACK` **返回 NULL**；栈由平台管 | 自动抬栈（App 的预算就失效了） |
| `task_exit_current` | **不返回** | 之后还有清理代码（执行不到） |

`caps()` 返回能力位：**没实现就返回 0**，别报"有"。报了的位必须真的有接口可调——16 位以上保留位读 0。

---

## 3. 这一层独有的坑（全是真机撞出来的）

| 现象 | 原因 | 做法 |
|---|---|---|
| 板上 `assert failed: xTaskCreatePinnedToCore ... xCoreID < 1` 并复位 | 代码写死绑 core 1，而 **ESP32-C3 是单核** | `const int cores = portNUM_PROCESSORS;` 再决定绑哪核（D46） |
| 短延时变成忙等、整机像卡住 | `pdMS_TO_TICKS(5)` 在 100 Hz 下 = 0 | 向上取整到至少 1 tick |
| `vTaskDelay(0)` 之后别的任务还是不动 | FreeRTOS 的 0 tick 不让出 | `sleep_ms(0)` 用 `taskYIELD()` |
| 返回给框架的任务句柄过一会儿变野指针 | 任务结构体被回收了 | 句柄结构体**故意不回收**（`host-fake` 与两块 IDF 端口都这样） |
| 打一行日志就 `Stack protection fault` | 1024 字节栈 + `vsnprintf` | App 侧下限已提到 3072；端口负责如实按 `stack_bytes` 建任务 |
| host 构建突然全红 | `find` 的排除路径带了 `./` | 用 `platform/<板>/*`，并加验证：`find ... \| grep -c <板>` 必须为 0 |

---

## 4. 新板接入清单

1. `platform/<板>/pf_espidf.c` + `.h`（只声明 `pf_platform_ops()`）+ `README.md`（实测规格 + 与别的板差别）。
2. **把九项照抄最接近的那块板，不抽公共层**（D42：板间差别恰是最易漂移处；第三块 IDF 板再议）。
3. 单测：`./build/test_pf_conformance`（host，14 条）。这份文件 host 与真机**同一份**。
4. 真机：
   ```bash
   source ~/.espressif/tools/activate_idf_v5.5.3.sh    # export.sh 会因 python env 缺失失败
   cd tests/port/idf && idf.py set-target <target> && idf.py build
   for p in $(lsof -t <port>); do kill -9 $p; done     # 杀掉上次 monitor 残留的 python，否则烧录报 ninja 失败
   idf.py -p <port> flash
   perl -e 'alarm 30; exec @ARGV' idf.py -p <port> monitor
   ```
   看到 `SUMMARY pass=14 fail=0 skip=0 test_port_conformance` 与 `DEVICE_DONE rc=0` 才算过。
5. 目标选择：`tests/port/idf/components/port_c3/CMakeLists.txt` 按 `idf_target` 选端口目录；
   新板要在这里加一条分支，并加 `sdkconfig.defaults.<target>`。
6. 文档：`docs/platform/<板>.md`（可选）、`实施步骤.md` §5/§8 各一行。

---

## 5. 烧录与串口（人授权才做）

- **设备连着 ≠ 授权**。每次烧录都要当次明确同意。
- C3：`/dev/cu.usbmodem101`（USB-JTAG，烧录与 printf 同一根线；**复位即断线**，所以固件写完
  `DEVICE_DONE` 停在原地不复位）。S3：`/dev/cu.usbserial-A5069RR4`（FT232R 桥）。
- 抓日志不能用 `cat`（设备在会话开始前就打完了）：用 `idf.py monitor`，它连上会复位一次，才有开头。
- `idf.py set-target` 会 fullclean + 把 `sdkconfig` 改写成 `sdkconfig.old`；`sdkconfig` 已进
  `.gitignore`，跑完不用还原。

---

## 6. 交付前自查

```bash
./build.sh run                                          # host 159 + 四道门禁，必须 rc=0
./build/test_pf_conformance                             # 或者点名跑这一份（先重编）
```

- 四行交付里 `Device tests` 只在**真跑过**时写 PASS，并写清板子型号与条数；没烧就写 NOT RUN。
- **不许把"编译通过"说成"硬件验证通过"**；也不许把"host 通过"当成"端口正确"。

---

## 7. 该读什么

| 想知道 | 读 |
|---|---|
| 九项的函数原型与逐项语义 | `platform/port/pf_ops.h`（**权威**） |
| 为什么没有 `destroy`/`realloc`/优先级 | `docs/pf-ops-decision.md` §13（被否决的方案） |
| 两份参考实现 | `platform/host-fake/pf_host_fake.c`（pthread）、`platform/ai-passport-c3/pf_espidf.c`（FreeRTOS） |
| 全局规矩、只有真机才暴露的坑 | [`AGENTS.md`](../../AGENTS.md) §7 |
| 命令细节、失败速查 | [`docs/build-and-usage.md`](../../docs/build-and-usage.md) §7/§10 |
