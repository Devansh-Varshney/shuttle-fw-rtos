/*
 * json.c
 *
 *  Created on: 19-May-2026
 *      Author: DevanshVarshney
 *
 *  JSON message handler for shuttle/wcs commands.
 *  See json.h for the architecture overview and the expected message format.
 */

#include "json.h"
#include "cJSON.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "portable.h"
#include "cmsis_compiler.h"   /* for __DMB() — CMSIS memory barrier intrinsic */
#include "mqtt_task.h"

#include <string.h>

/* ============================================================
 * Internal state
 * ============================================================ */

/* Queue item — holds one raw JSON message between enqueue and worker. */
struct json_msg {
    uint16_t len;
    /* +1 so we can safely null-terminate after copy. Real usable size
     * is JSON_MSG_MAX_SIZE - 1 for the payload. */
    uint8_t  buf[JSON_MSG_MAX_SIZE];
};

static osMessageQueueId_t s_json_queue = NULL;
static volatile bool      s_initialized = false;


static volatile int32_t  s_pp_x_pick  = 0;
static volatile int32_t  s_pp_y_pick  = 0;
static volatile int32_t  s_pp_x_place = 0;
static volatile int32_t  s_pp_y_place = 0;
static volatile uint32_t s_pp_valid   = 0;

/* Stats — single-word writes are atomic on Cortex-M7. */
static volatile uint32_t s_stat_received;
static volatile uint32_t s_stat_parsed;
static volatile uint32_t s_stat_parse_error;
static volatile uint32_t s_stat_queue_drop;

/* ============================================================
 * Forward declarations
 * ============================================================ */
static void json_worker_task(void *arg);
static void process_json(const char *json_str);

/* ============================================================
 * Init
 * ============================================================ */
void json_handler_init(void)
{
    if (s_initialized) return;
    s_initialized = true;

    /* Route cJSON allocations through FreeRTOS heap so they're thread-safe
     * and share the same pool. Must come BEFORE any cJSON usage. */
    cJSON_Hooks hooks = {
        .malloc_fn = pvPortMalloc,
        .free_fn   = vPortFree,
    };
    cJSON_InitHooks(&hooks);

    s_json_queue = osMessageQueueNew(JSON_MSG_QUEUE_DEPTH,
                                     sizeof(struct json_msg), NULL);

    static const osThreadAttr_t attr = {
        .name       = "jsonWorker",
        .stack_size = JSON_WORKER_STACK_BYTES,
        .priority   = osPriorityNormal,
    };
    osThreadNew(json_worker_task, NULL, &attr);
}

/* ============================================================
 * Enqueue (called from MQTT handler on tcpip_thread)
 * ============================================================ */
bool json_handler_enqueue(const void *data, size_t len)
{
    if (!s_initialized || !s_json_queue || !data) return false;
    /* Reserve one byte for null terminator. */
    if (len == 0 || len > (size_t)(JSON_MSG_MAX_SIZE - 1)) return false;

    struct json_msg msg;
    memcpy(msg.buf, data, len);
    msg.buf[len] = '\0';            /* cJSON needs a C string */
    msg.len = (uint16_t)len;

    if (osMessageQueuePut(s_json_queue, &msg, 0, 0) == osOK) {
        s_stat_received++;
        return true;
    }
    /* Queue full — drop message. */
    s_stat_queue_drop++;
    return false;
}

/* ============================================================
 * Worker task
 * ============================================================ */
static void json_worker_task(void *arg)
{
    (void)arg;
    struct json_msg msg;

    for (;;) {
        if (osMessageQueueGet(s_json_queue, &msg, NULL, osWaitForever) == osOK) {
            process_json((const char *)msg.buf);
        }
    }
}

/* ============================================================
 * Parse a single pick-place command JSON string
 *
 * Expected format:
 *     {"x_pick":<int>,"y_pick":<int>,"x_place":<int>,"y_place":<int>}
 *
 * Field order doesn't matter; extra fields are ignored.
 * All four fields are REQUIRED — partial messages are rejected.
 * ============================================================ */
static void process_json(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        s_stat_parse_error++;
        return;
    }

    cJSON *x_pick  = cJSON_GetObjectItemCaseSensitive(root, "x_pick");
    cJSON *y_pick  = cJSON_GetObjectItemCaseSensitive(root, "y_pick");
    cJSON *x_place = cJSON_GetObjectItemCaseSensitive(root, "x_place");
    cJSON *y_place = cJSON_GetObjectItemCaseSensitive(root, "y_place");

    /* All four fields must be present and numeric. */
    if (!cJSON_IsNumber(x_pick)  || !cJSON_IsNumber(y_pick)  ||
        !cJSON_IsNumber(x_place) || !cJSON_IsNumber(y_place)) {
        s_stat_parse_error++;
        cJSON_Delete(root);
        return;
    }

    /* Seqlock-style write: update fields, THEN bump the counter LAST.
     * Readers that see an unchanged counter before/after a read got a
     * coherent snapshot. Compiler must not reorder these writes; volatile
     * + the natural ordering of statements on Cortex-M (in-order writes
     * to same-region memory) guarantees this for our use case. */
    s_pp_x_pick  = x_pick->valueint;
    s_pp_y_pick  = y_pick->valueint;
    s_pp_x_place = x_place->valueint;
    s_pp_y_place = y_place->valueint;
    __DMB();                 /* Data Memory Barrier — ensure writes are visible */
    s_pp_valid++;

    s_stat_parsed++;

    /* We're done reading from the parsed tree — free it now so the next
     * step (encoding the response) doesn't double up on heap usage. */
    cJSON_Delete(root);

    /* Re-encode and forward the same values to "shuttle/forwaded". This
     * proves the full decode/encode cycle and lets a separate subscriber
     * see what we parsed. */
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddNumberToObject(resp, "x_pick",  s_pp_x_pick);
        cJSON_AddNumberToObject(resp, "y_pick",  s_pp_y_pick);
        cJSON_AddNumberToObject(resp, "x_place", s_pp_x_place);
        cJSON_AddNumberToObject(resp, "y_place", s_pp_y_place);
        cJSON_AddNumberToObject(resp, "valid",   (double)s_pp_valid);

        char *out = cJSON_PrintUnformatted(resp);
        if (out) {
            mqtt_app_publish("shuttle/forwaded", out, strlen(out), 1, false);
            cJSON_free(out);   /* free the printed string */
        }
        cJSON_Delete(resp);    /* free the response tree */
    }
}

/* ============================================================
 * Reader API
 * ============================================================ */
uint32_t json_get_last_pickplace(pickplace_cmd_t *out)
{
    if (!out) return 0;

    /* Seqlock read: keep retrying until the counter is stable
     * before and after the snapshot. Bounded retries to avoid
     * infinite spinning under pathological conditions. */
    for (int i = 0; i < 4; i++) {
        uint32_t v1 = s_pp_valid;
        __DMB();
        out->x_pick  = s_pp_x_pick;
        out->y_pick  = s_pp_y_pick;
        out->x_place = s_pp_x_place;
        out->y_place = s_pp_y_place;
        __DMB();
        uint32_t v2 = s_pp_valid;
        if (v1 == v2) {
            out->valid = v2;
            return v2;
        }
        /* writer updated mid-read; retry */
    }
    /* Gave up — return whatever we have with the current counter. */
    out->valid = s_pp_valid;
    return out->valid;
}

/* ============================================================
 * Stats accessors
 * ============================================================ */
uint32_t json_handler_get_received_count(void)     { return s_stat_received;    }
uint32_t json_handler_get_parsed_count(void)       { return s_stat_parsed;      }
uint32_t json_handler_get_parse_error_count(void)  { return s_stat_parse_error; }
uint32_t json_handler_get_queue_drop_count(void)   { return s_stat_queue_drop;  }
