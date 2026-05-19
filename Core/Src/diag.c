/*
 * diag.c
 *
 *  Periodic diagnostics publisher. See diag.h for design notes.
 */

#include "diag.h"
#include "main.h"          /* USR1_LED_Pin / USR2_LED_Pin and GPIO HAL */
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "portable.h"

#include "mqtt_task.h"
#include "json.h"

#include <stdio.h>
#include <string.h>

/* mqtt_is_connected() is currently commented out in mqtt_task.c, so we
 * read the visible status variable directly. It's a single volatile uint32_t
 * (atomic on Cortex-M7), so no lock is needed. */
extern volatile uint32_t mqtt_connect_status;

/* ============================================================
 * Internal state
 * ============================================================ */

static volatile bool s_initialized = false;

/* Static buffer for the diag JSON. Big enough for the full message
 * (~200 bytes typical). NOT on the task stack — keeps stack usage low. */
static char s_diag_buf[320];

/* ============================================================
 * LED helpers (active LOW: RESET = ON, SET = OFF)
 * ============================================================ */
static inline void led_usr1_on(void)   { HAL_GPIO_WritePin(USR1_LED_GPIO_Port, USR1_LED_Pin, GPIO_PIN_RESET); }
static inline void led_usr1_off(void)  { HAL_GPIO_WritePin(USR1_LED_GPIO_Port, USR1_LED_Pin, GPIO_PIN_SET); }
static inline void led_usr2_on(void)   { HAL_GPIO_WritePin(USR2_LED_GPIO_Port, USR2_LED_Pin, GPIO_PIN_RESET); }
static inline void led_usr2_off(void)  { HAL_GPIO_WritePin(USR2_LED_GPIO_Port, USR2_LED_Pin, GPIO_PIN_SET); }
static inline void led_usr2_toggle(void) { HAL_GPIO_TogglePin(USR2_LED_GPIO_Port, USR2_LED_Pin); }

/* ============================================================
 * Worker task
 * ============================================================ */
static void diag_task(void *arg)
{
    (void)arg;

    /* Initial LED state */
    led_usr1_off();
    led_usr2_off();

    for (;;) {
        osDelay(DIAG_PERIOD_MS);

        /* --- read everything we want to publish --- */
        size_t heap_free = xPortGetFreeHeapSize();
        size_t heap_min  = xPortGetMinimumEverFreeHeapSize();

        mqtt_stats_t mq;
        mqtt_get_stats(&mq);

        bool connected = (mqtt_connect_status == MQTT_STATUS_CONNECTED);

        uint32_t j_recv  = json_handler_get_received_count();
        uint32_t j_pars  = json_handler_get_parsed_count();
        uint32_t j_err   = json_handler_get_parse_error_count();
        uint32_t j_drop  = json_handler_get_queue_drop_count();

        pickplace_cmd_t pp;
        json_get_last_pickplace(&pp);

        uint32_t uptime = osKernelGetTickCount();

        /* --- build the JSON via snprintf (no heap alloc) --- */
        int n = snprintf(s_diag_buf, sizeof(s_diag_buf),
            "{"
              "\"up\":%lu,"
              "\"heap\":%lu,"
              "\"heap_min\":%lu,"
              "\"mqtt\":%d,"
              "\"m_conn\":%lu,"
              "\"m_disc\":%lu,"
              "\"m_fail\":%lu,"
              "\"m_tx\":%lu,"
              "\"m_rx\":%lu,"
              "\"m_drop\":%lu,"
              "\"m_pubto\":%lu,"
              "\"m_err\":%ld,"
              "\"j_rx\":%lu,"
              "\"j_ok\":%lu,"
              "\"j_err\":%lu,"
              "\"j_drop\":%lu,"
              "\"pp_v\":%lu"
            "}",
            (unsigned long)uptime,
            (unsigned long)heap_free,
            (unsigned long)heap_min,
            connected ? 1 : 0,
            (unsigned long)mq.connect_count,
            (unsigned long)mq.disconnect_count,
            (unsigned long)mq.connect_fail_count,
            (unsigned long)mq.tx_count,
            (unsigned long)mq.rx_count,
            (unsigned long)mq.tx_drop_count,
            (unsigned long)mq.pub_timeout_count,
            (long)mq.last_error,
            (unsigned long)j_recv,
            (unsigned long)j_pars,
            (unsigned long)j_err,
            (unsigned long)j_drop,
            (unsigned long)pp.valid
        );

        if (n > 0 && n < (int)sizeof(s_diag_buf)) {
            /* publish — non-blocking; if queue is full this drops harmlessly */
            mqtt_app_publish("shuttle/diag", s_diag_buf, (size_t)n, 1, false);
        }

        /* --- LEDs --- */
        /* USR1: solid ON when MQTT connected */
        if (connected) led_usr1_on(); else led_usr1_off();

        /* USR2: solid ON when heap is critically low; heartbeat otherwise */
        if (heap_free < DIAG_HEAP_WARN_BYTES) {
            led_usr2_on();    /* warning state */
        } else {
            led_usr2_toggle();    /* heartbeat — proves diag task is alive */
        }
    }
}

/* ============================================================
 * Public init
 * ============================================================ */
void diag_init(void)
{
    if (s_initialized) return;
    s_initialized = true;

    static const osThreadAttr_t attr = {
        .name       = "diagTask",
        .stack_size = DIAG_TASK_STACK_BYTES,
        .priority   = osPriorityLow,   /* low priority — diag is best-effort */
    };
    osThreadNew(diag_task, NULL, &attr);
}
