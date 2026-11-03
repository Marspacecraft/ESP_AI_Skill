# 跨仓库合并：一份框架，两个仓库

**结论先说**：本仓库只提供**机制**（`api/` + `framework/` + `platform/` + `common/`），
**不提供任何产品**。产品仓库（例如 `ai-passport`）把本仓库当作一个组件引进来，
自带 App、系统 App 与真机测试。两边各有各的门禁，靠**头文件**而不是源码耦合。

这不是设计洁癖，是 `policy.txt` 已经写下的分层：`apps/system/` 与 `tests/sys/` 属于 tag 1（系统 App），
而本仓库现在**一个都没建**——它们本来就该在产品仓库里。

## 谁写哪里

| 位置 | 归谁 | 本仓库现状 |
|---|---|---|
| `api/`、`framework/`、`platform/`、`common/` | 本仓库（tag 3/4/5） | 已实现 |
| `apps/demo/`（样板 App） | 本仓库（tag 2） | 已实现，80 行 |
| `apps/app_manifest.c`（注册清单） | tag 1 与 tag 2 **共写、只许追加** | 已实现（只有 demo） |
| `apps/system/`（系统 App、服务宿主） | 产品仓库（tag 1） | **空**——本仓库不建 |
| `tests/sys/` | 产品仓库（tag 1） | **空** |
| `docs/sys/` | 产品仓库（tag 1） | **空** |

`policy.txt` 里这四条一直配着，所以产品仓库把目录建起来后，**本仓库的门禁不用改一行**
就会开始检查它们（这是"只约束已配置目录"的直接好处）。

## 怎么引进来

产品仓库按组件的方式引（`ai-passport` 已经跑通这条路）：

```cmake
# product/components/esp_ai_skill/CMakeLists.txt（要点）
set(EAIS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../ESP_AI_Skill" CACHE PATH)
file(GLOB EAIS_SRCS ${EAIS_ROOT}/common/src/*.c ${EAIS_ROOT}/framework/src/*.c)
list(FILTER EAIS_SRCS EXCLUDE REGEX "/fw_main\\.c$")   # 整机 app_main 归产品，不归框架
list(APPEND EAIS_SRCS ${EAIS_ROOT}/platform/<板>/pf_espidf.c)
idf_component_register(SRCS ${EAIS_SRCS}
                       INCLUDE_DIRS ${EAIS_ROOT} ${EAIS_ROOT}/platform/port
                       REQUIRES esp_timer)
```

产品侧的 `app_main()` 里调一次 `fw_embed_start(&apps_manifest)`：
- 成功 → 框架接管，系统 App 起来；
- 失败 → 返回 `cm_err_t`，**停不停由产品决定**（整机固件才"停在原地等看门狗"）。

**必须排除 `fw_main.c`**：它在框架里提供"整机入口"，会和产品自己的 `app_main()` 撞符号。
`fw_startup()` 已经从 `fw_main.c` 拆到 `framework/src/fw_startup.c`（D37），
所以排除整机入口不会连启动序列一起丢掉——这条是 2026-11-03 在真机上被链接错误逼出来的。

## 合并时最容易出问题的三处

1. **`app_manifest.c` 是共写的**。本仓库给 `s_apps[]` 一个初值，产品往里追加。
   所以它**只许追加**：重排会让两边的 diff 互相覆盖，而且不报错。
2. **两边的 `app_main()` 只能有一个**。`tests/tools/check_entry_unique.sh` 只在本仓库内判；
   产品仓库有它自己的入口，本仓库的自检固件（`tests/port/idf`）又各有一个——
   所以那条门禁对 `tests/` 目录是豁免的（D 见 `实施步骤.md` §7）。
3. **两边各有一套构建**。本仓库的 `build.sh` 是 **host** 构建（cc + pthread，用于跑单元测试）；产品仓库是 ESP-IDF。
   两边都编同一份源码，所以平台端口要能被 host 构建排除：
   `build.sh` 里 `! -path 'platform/ai-passport-c3/*' ! -path 'platform/esp32s3-devkit/*'`。

## 现在的证据

- `ai-passport`（ESP32-C3）把本仓库当组件接进来，自检 14 + 9 + 6 + 8 条在**它的固件里**跑绿，
  随后产品自己的 BSP demo 照常启动。
- 本仓库的 `tests/port/idf/` 是**独立固件**，按 `IDF_TARGET` 选端口，C3 与 S3 各跑同一份 37 条。
- 两边都没有为了让对方通过而修改对方的源码——只加了组件目录与一次 `fw_embed_start()` 调用。

## 还没做的

- 反向合并（把产品仓库的系统 App 拉回本仓库做样板）**没有做**，也不打算做：
  它们依赖具体产品的服务与界面，拉回来只会让本仓库长出一个"只服务于一块板"的目录。
- 两边门禁的**对账**没有自动化：产品的 `validate.sh` 与本仓库的 `build.sh` 各跑各的，
  目前靠人（"两边都绿"）而不是靠机器保证。
