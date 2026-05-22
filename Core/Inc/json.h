/*
 * json.h
 *
 *  Created on: 19-May-2026
 *      Author: DevanshVarshney
*/

#ifndef INC_JSON_H_
#define INC_JSON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JSON_MSG_MAX_SIZE
#define JSON_MSG_MAX_SIZE       256
#endif

#ifndef JSON_MSG_QUEUE_DEPTH
#define JSON_MSG_QUEUE_DEPTH    32
#endif

#ifndef JSON_WORKER_STACK_BYTES
#define JSON_WORKER_STACK_BYTES (4 * 1024)
#endif

/* Parsed pick-place command. */
typedef struct {
    int32_t  x_pick;
    int32_t  y_pick;
    int32_t  x_place;
    int32_t  y_place;
    uint32_t valid;     /* monotonic counter; increments on each successful
                         * parse. Read it to detect new commands. */
} pickplace_cmd_t;


/* Initialize: routes cJSON allocations through FreeRTOS heap, creates the
 * queue, spawns the JSON worker task. Idempotent — safe to call twice. */
void json_handler_init(void);

/* Enqueue raw JSON bytes for parsing. Safe from any task (including MQTT
 * handlers on tcpip_thread). Non-blocking; returns false on queue full,
 * invalid args, or before init. */
bool json_handler_enqueue(const void *data, size_t len);

/* Read the most recently parsed pick-place command into the caller's buffer.
 * Safe to call from any task. Returns the `valid` counter so the caller can
 * detect updates between reads (compare to the previous value). */
uint32_t json_get_last_pickplace(pickplace_cmd_t *out);

/* Diagnostics (single-word atomic reads). */
uint32_t json_handler_get_received_count(void);
uint32_t json_handler_get_parsed_count(void);
uint32_t json_handler_get_parse_error_count(void);
uint32_t json_handler_get_queue_drop_count(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_JSON_H_ */
