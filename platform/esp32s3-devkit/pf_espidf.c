/**
 * @file pf_espidf.c
 * @brief ESP32-S3 开发板（ESP-IDF 5.5.3）的九项端口实现。
 *
 * 设计要点：
 * - 九项语义全部照 `platform/port/pf_ops.h` 的注释落，**不解释、不改写**。
 * - 任务句柄按"只分配不回收"处理：框架会一直拿着它，回收等于给框架一个
 *   悬空指针（host-fake 同样如此）。
 * - 栈按字节收下：ESP-IDF 的 `xTaskCreate()` 收的是 word 数，这里换算。
 * - 优先级不来自 App：平台自己挑一个与 app_main 同档的值。
 * - **双核**：`xTaskCreate()` 不绑核，任务落在哪个核由调度器决定。框架里
 *   跨任务的状态因此只能靠 `__atomic_*`（D20）保证可见性——`volatile`
 *   在单核 C3 上还能蒙对，在 S3 上不能。`taskYIELD()` 也只让当前核。
 */
#include "pf_espidf.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

/** 框架任务用的优先级。App 不决定优先级，平台给一个固定档。
 *  双核上这仍然是"高于 IDLE、低于系统任务"的一档，不绑核。 */
#define ESP_PORT_TASK_PRIO (tskIDLE_PRIORITY + 5)

struct pf_mutex {
    SemaphoreHandle_t h;
};

struct pf_task {
    TaskHandle_t h;
};

typedef struct {
    void (*entry)(void *);
    void *arg;
} esp_port_task_boot_t;

/* ---------------- 内存 ---------------- */

static void *esp_port_mem_alloc(size_t bytes)
{
    if (bytes == 0u) {
        return NULL;
    }
    return malloc(bytes);
}

static void esp_port_mem_free(void *p)
{
    free(p);
}

/* ---------------- 时钟与延时 ---------------- */

static uint32_t esp_port_clock_ms(void)
{
    /* esp_timer_get_time() 是 64 位微秒；截断成 uint32_t 后约 49.7 天回绕，
     * 由调用方用差值比较处理（见 pf_ops.h 的 clock_ms 注释）。 */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void esp_port_sleep_ms(uint32_t ms)
{
    TickType_t ticks;

    if (ms == 0u) {
        /* 契约：ms==0 也要让出一次。vTaskDelay(0) 不让出，必须 taskYIELD()。 */
        taskYIELD();
        return;
    }

    ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0u) {
        /* 100 Hz tick 下 1~9ms 会被截成 0：宁可多睡一个 tick，也不能变成忙等。 */
        ticks = 1u;
    }
    vTaskDelay(ticks);
}

/* ---------------- 互斥 ---------------- */

static pf_mutex_t *esp_port_mutex_create(void)
{
    pf_mutex_t *m = (pf_mutex_t *)malloc(sizeof(*m));

    if (m == NULL) {
        return NULL;
    }
    m->h = xSemaphoreCreateMutex();
    if (m->h == NULL) {
        free(m);
        return NULL;
    }
    return m;
}

static int esp_port_mutex_take(pf_mutex_t *m, uint32_t timeout_ms)
{
    TickType_t ticks;

    if (m == NULL) {
        return -1;
    }
    if (timeout_ms == 0u) {
        return (xSemaphoreTake(m->h, 0) == pdTRUE) ? 0 : -1;
    }
    /* 向上取整：超时宁可长一点点，也不能比调用方给的短。 */
    ticks = (TickType_t)(((uint64_t)timeout_ms * configTICK_RATE_HZ + 999u) / 1000u);
    if (ticks == 0u) {
        ticks = 1u;
    }
    return (xSemaphoreTake(m->h, ticks) == pdTRUE) ? 0 : -1;
}

static void esp_port_mutex_give(pf_mutex_t *m)
{
    if (m != NULL) {
        (void)xSemaphoreGive(m->h);
    }
}

/* ---------------- 任务 ---------------- */

static void esp_port_task_trampoline(void *p)
{
    esp_port_task_boot_t boot = *(esp_port_task_boot_t *)p;

    free(p); /* 启动参数是分配出来的，进任务后立刻还掉 */
    boot.entry(boot.arg);

    /* App 的任务函数返回 = 自认挂了。框架的 task() 不该返回，这里兜底收尾。 */
    vTaskDelete(NULL);
}

static pf_task_t *esp_port_task_create(const pf_task_desc_t *desc)
{
    pf_task_t *t;
    esp_port_task_boot_t *boot;

    if (desc == NULL || desc->entry == NULL) {
        return NULL;
    }
    if (desc->stack_bytes < PF_TASK_MIN_STACK) {
        return NULL; /* 契约：不许自动抬栈 */
    }

    t = (pf_task_t *)calloc(1u, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    boot = (esp_port_task_boot_t *)malloc(sizeof(*boot));
    if (boot == NULL) {
        free(t);
        return NULL;
    }
    boot->entry = desc->entry;
    boot->arg = desc->arg;

    if (xTaskCreate(esp_port_task_trampoline, (desc->name != NULL) ? desc->name : "app",
                    (uint32_t)(desc->stack_bytes / sizeof(StackType_t)), boot,
                    ESP_PORT_TASK_PRIO, &t->h) != pdPASS) {
        free(boot);
        free(t);
        return NULL;
    }

    /* 故意不回收 t：框架会一直拿着这个句柄。 */
    return t;
}

static void esp_port_task_exit_current(void)
{
    /* 删自己：本函数不再返回（TCB 由 IDLE 任务回收）。 */
    vTaskDelete(NULL);
}

/* ---------------- 能力位 ---------------- */

static uint32_t esp_port_caps(void)
{
    /* 这块板有 8 MB PSRAM、16 MB Flash、Wi-Fi、BLE，但**本仓库没有任何一个能力
     * 头**（api/af_api/disp.h 等还不存在）。按 architecture §7.3 的裁决：
     * "查得到硬件 ≠ 有接口可调"，没有可调接口就不许点亮能力位。 */
    return 0u;
}

/* ---------------- 表 ---------------- */

const pf_ops_t pf_espidf_ops = {
    .version = PF_OPS_VERSION,
    .caps = esp_port_caps,
    .mem_alloc = esp_port_mem_alloc,
    .mem_free = esp_port_mem_free,
    .clock_ms = esp_port_clock_ms,
    .sleep_ms = esp_port_sleep_ms,
    .mutex_create = esp_port_mutex_create,
    .mutex_take = esp_port_mutex_take,
    .mutex_give = esp_port_mutex_give,
    .task_create = esp_port_task_create,
    .task_exit_current = esp_port_task_exit_current,
};

const pf_ops_t *pf_platform_ops(void)
{
    return &pf_espidf_ops;
}

void pf_platform_log(const char *line)
{
    if (line != NULL) {
        printf("%s\n", line);
        (void)fflush(stdout);
    }
}
