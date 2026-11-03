/**
 * @file idf_main.c
 * @brief 真机侧的入口：跑一遍端口契约套件，再做一次"日志栈够不够"的探针。
 *
 * 结果判定不靠人看屏幕，靠串口上那几行 `PASS / FAIL / SUMMARY / PROBE`——
 * 和 host 侧是同一套行式输出，所以"真机通过"可以像 host 一样被机器判定。
 *
 * 打完结果**不退不重启**：USB Serial/JTAG 复位会断开重连，抓日志的那一侧
 * 容易漏掉开头几行。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "framework/include/fw_app.h"
#include "platform/port/pf_ops.h"

/* tinytest 生成的入口：tests/port/test_port_conformance.c 里的 `main`。
 * ESP-IDF 的应用入口叫 `app_main`，所以这里直接调 `main` 不冲突。 */
int main(void);

/* ------------------------------------------------------------------ */
/* 日志栈探针                                                          */
/* ------------------------------------------------------------------ */

static int g_probe_done;

/**
 * 在一张**刚好 APP_STACK_MIN 大小**的栈上打一行最长的日志。
 *
 * 为什么值得单独探一次：`fw_vlog()` 在栈上有 256 字节缓冲，再叠 newlib 的
 * `vsnprintf()`（它会自带锁与浮点相关的状态），这两者加起来很可能超过 1 KB。
 * 而 App 的主任务栈就是 `APP_STACK_MIN`——也就是说，"能打日志"是 App 能不能
 * 用的最低门槛，不能靠"host 上没崩"来推。
 */
static void log_probe_task(void *arg)
{
    char big[120];
    int *flag = (int *)arg;

    memset(big, 'x', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';

    /* 故意把三样东西一次塞满：定长缓冲、数字格式化、长字符串。 */
    fw_logp("probe", "数字=%d 字符串=%s 长文=%s", 42, "abc", big);
    fw_log("无标签的一行：%u/%s", (unsigned)0xFFFFFFFFu, "end");

    __atomic_store_n(flag, 1, __ATOMIC_RELAXED);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void run_probe(uint32_t stack_bytes)
{
    pf_task_desc_t d;
    int *flag = &g_probe_done;

    __atomic_store_n(flag, 0, __ATOMIC_RELAXED);
    d.name = "logprobe";
    d.entry = log_probe_task;
    d.arg = flag;
    d.stack_bytes = stack_bytes;

    printf("PROBE begin stack=%u (APP_STACK_MIN=%u)\n", (unsigned)stack_bytes,
           (unsigned)APP_STACK_MIN);
    (void)fflush(stdout);

    if (pf_platform_ops()->task_create(&d) == NULL) {
        printf("PROBE create_failed stack=%u\n", (unsigned)stack_bytes);
        (void)fflush(stdout);
        return;
    }

    for (int i = 0; i < 100 && __atomic_load_n(flag, __ATOMIC_RELAXED) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("PROBE %s stack=%u\n",
           (__atomic_load_n(flag, __ATOMIC_RELAXED) != 0) ? "survived" : "LOST",
           (unsigned)stack_bytes);
    (void)fflush(stdout);
}

void app_main(void)
{
    int rc;

    printf("\n=== ESP_AI_Skill 端口契约（ESP32-C3 真机）===\n");
    (void)fflush(stdout);

    rc = main();

    printf("DEVICE_DONE rc=%d\n", rc);
    (void)fflush(stdout);

    /* 先验 2048（预期通过），再验 1024（预期可能炸）。这样一次上电拿到两行结论。 */
    run_probe(2048u);
    run_probe(APP_STACK_MIN);

    printf("PROBE_ALL_DONE\n");
    (void)fflush(stdout);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
