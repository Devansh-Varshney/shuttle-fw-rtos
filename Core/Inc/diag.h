/*
 * diag.h
 *
 *  Periodic diagnostics publisher for the MQTT shuttle firmware.
 *
 *  Publishes a small JSON blob to "shuttle/diag" every DIAG_PERIOD_MS ms
 *  containing heap, MQTT, and JSON-handler counters. Designed to be
 *  resilient to heap exhaustion: builds the JSON with snprintf into a
 *  static buffer (NO cJSON, NO malloc on the hot path).
 *
 *  Also drives two indicator LEDs:
 *    USR1 LED — solid ON when MQTT connected, OFF otherwise
 *    USR2 LED — toggles every diag tick (heartbeat)
 *
 *  If USR2 stops blinking, the diag task itself is frozen (likely heap
 *  starvation or stack overflow). If USR2 keeps blinking but the diag
 *  topic on the broker stops updating, MQTT publishing is stuck.
 */

#ifndef INC_DIAG_H_
#define INC_DIAG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DIAG_PERIOD_MS
#define DIAG_PERIOD_MS         5000   /* publish every 5 seconds */
#endif

#ifndef DIAG_TASK_STACK_BYTES
#define DIAG_TASK_STACK_BYTES  (2 * 1024)
#endif

/* Heap warning threshold — if free heap drops below this, USR2 lights
 * solid (instead of heartbeat-toggling) so you can see at-a-glance. */
#ifndef DIAG_HEAP_WARN_BYTES
#define DIAG_HEAP_WARN_BYTES   5000
#endif

/* Initialize: spawns the diag task. Idempotent. */
void diag_init(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_DIAG_H_ */
