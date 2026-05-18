/*
 * mqtt_task.h
 *
 *  Created on: 13-May-2026
 *      Author: DevanshVarshney
 *
 *  Generic MQTT API for embedded firmware. Thin layer over lwIP's raw
 *  MQTT client + a FreeRTOS task. All public functions are safe to call
 *  from any task (NOT from ISRs).
 *
 *  Threading rules (READ ME):
 *    - mqtt_init must be called once, single-threaded, before MQTT use.
 *    - All other public functions are safe from any task. Not from ISRs.
 *    - User-supplied message handlers (mqtt_msg_handler_t) RUN ON
 *      tcpip_thread. They MUST be fast (< 1 ms), non-blocking, no
 *      printf-to-slow-UART, no long mutex holds. Defer real work to
 *      your own task via a queue/semaphore.
 *    - State-change callbacks (mqtt_state_cb_t) may run on either
 *      tcpip_thread or the MQTT worker task. Same restrictions.
 *
 *  Not supported:
 *    - TLS / MQTT over 8883 (use altcp_mqtt + mbedTLS).
 *    - Per-publish delivery confirmation API (PUBACK fires internally
 *      and is reflected in mqtt_get_stats only).
 *    - ISR-context API entry (osMessageQueuePut is ISR-safe but the
 *      bounded-copy and validation around it make ISR use a bad idea).
 */

#ifndef INC_MQTT_TASK_H_
#define INC_MQTT_TASK_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * mqtt_connect_status values (exported for Live Expressions / app code)
 * ============================================================ */
#define MQTT_STATUS_IDLE              0u
#define MQTT_STATUS_CONNECTED         1u
#define MQTT_STATUS_CONNECTING        2u   /* connect attempt in flight; waiting for CONNACK */
#define MQTT_STATUS_BAD_IP            98u   /* broker_ip failed to parse */
#define MQTT_STATUS_OOM               99u   /* mqtt_client_new returned NULL */
#define MQTT_STATUS_REFUSED_BASE      100u  /* + MQTT_CONNECT_REFUSED_* (1..5) */
#define MQTT_STATUS_DISCONNECTED      356u  /* 100 + MQTT_CONNECT_DISCONNECTED (256) */
#define MQTT_STATUS_CONNECT_ERR_BASE  200u  /* + (-err_t) on sync mqtt_client_connect fail */

/* ============================================================
 * Types
 * ============================================================ */

/* MQTT subsystem configuration. Strings are NOT copied -- they must outlive
 * the MQTT subsystem (string literals are fine). */
typedef struct {
    const char *broker_ip;        /* e.g. "192.168.0.37" */
    uint16_t    broker_port;      /* e.g. 1883 */
    const char *client_id;        /* must be unique per device */
    uint16_t    keep_alive_s;     /* 60 is typical */
    const char *username;         /* NULL if not using auth */
    const char *password;         /* NULL if not using auth */
} mqtt_config_t;

/* Inbound-message handler. Runs on tcpip_thread. KEEP IT FAST. */
typedef void (*mqtt_msg_handler_t)(const char *topic,
                                   const void *payload, size_t len,
                                   void *user_ctx);

/* Connection-state observer; fires on transitions only. */
typedef void (*mqtt_state_cb_t)(bool connected);

/* Runtime statistics snapshot. */
typedef struct {
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t connect_fail_count;
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t tx_drop_count;       /* publishes dropped because queue was full */
    uint32_t rx_truncated_count;  /* inbound messages dropped: too large to buffer */
    uint32_t pub_timeout_count;   /* publishes dropped: inflight slot stuck */
    int32_t  last_error;
} mqtt_stats_t;

/* ============================================================
 * Lifecycle
 * ============================================================ */

void mqtt_init(const mqtt_config_t *cfg);
bool mqtt_is_connected(void);
void mqtt_set_state_callback(mqtt_state_cb_t cb);
void mqtt_force_reconnect(void);

/* ============================================================
 * Publishing
 *
 * NOTE: mqtt_app_publish_printf uses ~256 bytes of CALLER stack for the
 *       format buffer. Tasks with small stacks must use mqtt_app_publish
 *       with their own (smaller) buffer instead.
 * ============================================================ */

bool mqtt_app_publish(const char *topic, const void *data, size_t len,
                      uint8_t qos, bool retain);
bool mqtt_app_publish_string(const char *topic, const char *str);
bool mqtt_app_publish_printf(const char *topic, const char *fmt, ...);
bool mqtt_app_publish_retained(const char *topic, const void *data, size_t len);

/* ============================================================
 * Subscribing
 *
 * Supports MQTT wildcards:
 *   '+' matches exactly one topic level   ("a/+/c" matches "a/b/c")
 *   '#' matches zero or more levels at end ("a/#" matches "a", "a/b", "a/b/c")
 * ============================================================ */

bool mqtt_app_subscribe(const char *topic, uint8_t qos,
                        mqtt_msg_handler_t handler, void *user_ctx);
bool mqtt_app_unsubscribe(const char *topic);
void mqtt_set_default_handler(mqtt_msg_handler_t h, void *user_ctx);

/* ============================================================
 * Last Will and Testament
 * ============================================================ */

/* Returns true if the will was accepted (or cleared via topic==NULL).
 * On false, the previously-set will (if any) is preserved unchanged. */
bool mqtt_set_will(const char *topic, const void *payload, size_t len,
                   uint8_t qos, bool retain);

/* ============================================================
 * Diagnostics
 * ============================================================ */

void mqtt_get_stats(mqtt_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* INC_MQTT_TASK_H_ */
