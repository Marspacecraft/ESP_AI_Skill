# api/af_api/ —— App 能 link 的全部头

这是 App 唯一的门面。**五个头 + 一个不在这里的公共类型头**：

| 头 | 里面有什么 |
|---|---|
| `app.h` | `app_ctx_t`、`app_desc_t`、`fw_manifest_t`、三个回调类型、`APP_*` 常量，以及使用面 `fw_delay_block` / `fw_alloc` / `fw_free` / `fw_log` / `fw_logp` |
| `res.h` | `res_acquire` / `res_release` / `res_kind_e` / `res_life_e`，配额三件 `fw_res_set_quota` / `fw_res_revoke` / `fw_res_usage` |
| `evt.h` | `fw_event_declare` / `post` / `post_isr` / `wait` / `take`，`evt_topic_t` |
| `svc.h` | 服务注册表 `fw_svc_register/unregister/ready` + 提交型 `fw_svc_submit/submit_take/submit_count` |
| `mgr.h` | **管理器面（控制面）**：`fw_alloc_id` / `fw_app_ctx_new*` / `fw_app_start` / `fw_app_request_cancel` / `fw_app_state` / `fw_app_id` / `fw_live_*` / `fw_app_at`，以及 `fw_app_state_e`。普通 App 不需要它 |

`cm_err_t` 与错误码在 `common/include/cm_types.h`（放 `api/` 任何一支都会造成反向依赖）。

**没有的能力头**：显示、输入、音频、存储、网络、电源。设计稿里出现过 `disp_commit` 一类的名字，
它们现在**不可调用**——查得到硬件（`PF_CAP_*`）不等于有接口。能力位与接口头的对应关系见
`docs/generated/catalog.md`，那份表与代码双向绑定，写歪了 `./build.sh run` 会红。

谁写：tag 5。App 只看这里，不看 `framework/`、不看 `platform/`。
