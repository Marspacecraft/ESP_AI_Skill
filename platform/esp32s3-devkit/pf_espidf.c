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
#include "driver/uart.h"

#include "pf_cap.h"  /* platform/port 已在 include 路径上（与 pf_ops.h 相同） */
#include "pf_uart.h"

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

/* ── 应用串口（PF_CAP_UART，见 platform/port/pf_uart.h） ─────────────
 * 用 UART1（UART0 被控制台占着）。引脚是**板级约定**，不是框架约定：
 *   TX = GPIO17，RX = GPIO18
 * 换板/换个插座就把下面两个宏改成你那块板真正引出来的两根线。
 * 它们没接到外设时也能验：写出去没人收，`read` 会如实超时。
 */
#define UART_APP_PORT   UART_NUM_1
#define UART_APP_TX     17
#define UART_APP_RX     18

static int s_uart_opened;

static int app_uart_open(uint32_t port, uint32_t baud)
{
    /* 必须**整体清零**再逐字段赋值：`uart_config_t` 里还有 `rx_flow_ctrl_thresh`
     * 这种"硬件流控关掉时按 0 校验"的字段，未初始化时它是栈上的垃圾值——
     * S3 上实测会直接 `E uart: uart_param_config(995): rx flow thresh error`，
     * 而 C3 上碰巧是 0 所以一路绿灯。**"在 C3 上过了"不等于"这份代码是对的"。** */
    uart_config_t cfg = { 0 };
    esp_err_t err;

    if (port != 0u || baud == 0u || s_uart_opened) {
        return -1;
    }
    err = uart_driver_install(UART_APP_PORT, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return -1;
    }
    cfg.baud_rate = (int)baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    err = uart_param_config(UART_APP_PORT, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(UART_APP_PORT, UART_APP_TX, UART_APP_RX,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        (void)uart_driver_delete(UART_APP_PORT);
        return -1;
    }
    s_uart_opened = 1;
    return 0;
}

static int app_uart_close(uint32_t port)
{
    if (port != 0u || !s_uart_opened) {
        return -1;
    }
    if (uart_driver_delete(UART_APP_PORT) != ESP_OK) {
        return -1;
    }
    s_uart_opened = 0;
    return 0;
}

static int app_uart_write(uint32_t port, const void *buf, size_t len, uint32_t timeout_ms)
{
    int n;

    (void)timeout_ms; /* TX 缓冲满时 uart_write_bytes 自己等；框架已按 32 字节切片 */
    if (port != 0u || !s_uart_opened || buf == NULL) {
        return -1;
    }
    n = uart_write_bytes(UART_APP_PORT, buf, len);
    return (n < 0) ? -1 : n;
}

static int app_uart_read(uint32_t port, void *buf, size_t cap, uint32_t timeout_ms)
{
    int n;

    if (port != 0u || !s_uart_opened || buf == NULL || cap == 0u) {
        return -1;
    }
    n = uart_read_bytes(UART_APP_PORT, buf, cap, pdMS_TO_TICKS(timeout_ms));
    return (n < 0) ? -1 : n;
}

static const pf_uart_ops_t s_uart_ops = {
    .version = PF_UART_OPS_VERSION,
    .count = 1u,
    .open = app_uart_open,
    .close = app_uart_close,
    .write = app_uart_write,
    .read = app_uart_read,
};

static uint32_t esp_port_caps(void)
{
    /* 只点亮**已经有接口头**的能力：目前只有串口（`api/af_api/uart.h`）。
     * PSRAM/Flash/Wi-Fi/BLE 都在板上，但本仓库没有对应能力头，
     * 按 architecture §7.3「查得到硬件 ≠ 有接口可调」，一概不报。 */
    return PF_CAP_UART;
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

void pf_platform_boot(void)
{
    /* 板级启动钩子：把能力实现交上去。注册失败的位就是"没有这个能力"，
     * 之后 cap_available(PF_CAP_UART) 会如实回答 false。 */
    (void)fw_cap_register(PF_CAP_UART, &s_uart_ops);
}
