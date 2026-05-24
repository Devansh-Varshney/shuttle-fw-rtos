/*
 * mqtt_task.c
 *
 *  Created on: 13-May-2026
 *      Author: DevanshVarshney
 *
 *  Generic MQTT API over lwIP raw client + FreeRTOS (CMSIS-RTOS v2).
 *  All lwIP calls are marshalled to tcpip_thread via tcpip_callback().
 */

#include "mqtt_task.h"

#include "cmsis_os.h"
#include "lwip/apps/mqtt.h"
#include "lwip/apps/mqtt_priv.h"   /* private: gives us access to client->conn so we can abort */
#include "lwip/altcp.h"            /* altcp_abort */
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ============================================================
 * Compile-time limits (tune to your needs)
 * ============================================================ */
#define MQTT_MAX_SUBS               16
#define MQTT_PUB_QUEUE_DEPTH        32
#define MQTT_TOPIC_BUF_SIZE         128
#define MQTT_PAYLOAD_BUF_SIZE       256
#define MQTT_WILL_PAYLOAD_SIZE      64

#define MQTT_TASK_STACK_BYTES       (1024 * 4)
#define MQTT_LOOP_PERIOD_MS         100
#define MQTT_LINK_SETTLE_MS         500
#define MQTT_PHY_WARMUP_MS          500

/* Exponential backoff for reconnect attempts. Streak doubles backoff:
 *   1s, 2s, 4s, 8s, 16s, 32s, 60s (cap). Reset on successful connect. */
#define MQTT_BACKOFF_INITIAL_MS     1000u
#define MQTT_BACKOFF_MAX_MS         60000u

/* If a publish in-flight slot is not released within this window we
 * assume the previous publish is wedged, force a reconnect. */
#define MQTT_PUB_TIMEOUT_MS         10000u

/* If we're stuck in MQTT_STATUS_CONNECTING (no CONNACK and no DISCONNECTED
 * notification from lwIP) for this long, force-tear-down and retry.
 * Should be longer than lwIP's TCP SYN retry budget (~75-90 s default) so
 * that on a dead broker we let lwIP report the failure naturally. */
#define MQTT_CONNECT_TIMEOUT_MS     30000u

/* Guard against future maintainers bumping payload size into stack-overflow
 * territory: mqtt_app_publish_printf allocates a buffer this big on the
 * CALLER's stack. Keep it sane. */
_Static_assert(MQTT_PAYLOAD_BUF_SIZE <= 512,
               "MQTT_PAYLOAD_BUF_SIZE too large for caller-stack printf buffer; "
               "use a static buffer + mutex if you really need >512 bytes");

/* ============================================================
 * Live-Expressions-visible state.
 *
 * WARNING: These globals are written by tcpip_thread during message
 *          assembly. Application code must NOT read them - the bytes
 *          tear during reassembly. They exist solely for inspection
 *          via the debugger (Live Expressions / variables view).
 *          Use a subscribe handler (mqtt_app_subscribe) to safely
 *          consume inbound messages from application code.
 * ============================================================ */
volatile uint32_t mqtt_connect_status   = MQTT_STATUS_IDLE;
volatile uint32_t mqtt_rx_msg_count     = 0;
volatile uint32_t mqtt_tx_msg_count     = 0;
volatile int32_t  mqtt_last_publish_err = 0;
volatile uint16_t mqtt_last_payload_len = 0;
char              mqtt_last_topic[MQTT_TOPIC_BUF_SIZE]     = {0};
char              mqtt_last_payload[MQTT_PAYLOAD_BUF_SIZE] = {0};

/* Statistics. Word-sized writes are atomic on Cortex-M7. */
static volatile uint32_t s_stat_connect_count;
static volatile uint32_t s_stat_disconnect_count;
static volatile uint32_t s_stat_connect_fail_count;
static volatile uint32_t s_stat_tx_count;
static volatile uint32_t s_stat_rx_count;
static volatile uint32_t s_stat_tx_drop_count;
static volatile uint32_t s_stat_rx_truncated_count;
static volatile uint32_t s_stat_pub_timeout_count;
static volatile int32_t  s_stat_last_error;

struct sub_entry {
    bool                 used;
    bool                 needs_resubscribe;   /* set on add or on reconnect */
    bool                 pending_unsub;       /* set on unsubscribe; cleared after broker ack */
    char                 topic[MQTT_TOPIC_BUF_SIZE];
    uint8_t              qos;
    mqtt_msg_handler_t   handler;
    void                *user_ctx;
};

struct pub_req {
    char     topic[MQTT_TOPIC_BUF_SIZE];
    uint8_t  payload[MQTT_PAYLOAD_BUF_SIZE];
    uint16_t len;
    uint8_t  qos;
    bool     retain;
};

struct will_cfg {
    bool     used;
    char     topic[MQTT_TOPIC_BUF_SIZE];
    uint8_t  payload[MQTT_WILL_PAYLOAD_SIZE];
    size_t   len;
    uint8_t  qos;
    bool     retain;
};



static volatile bool      s_initialized = false;
static mqtt_config_t      s_cfg;

static mqtt_client_t     *s_client      = NULL;
static ip_addr_t          s_broker_addr;
static uint16_t           s_rx_offset   = 0;
static bool               s_rx_skip     = false;  /* current inbound message exceeds buffer; drop it */

static osMessageQueueId_t s_pub_queue     = NULL;
static struct pub_req     s_inflight;
static osSemaphoreId_t    s_inflight_free = NULL;

static osMutexId_t        s_subs_mutex     = NULL;
static struct sub_entry   s_subs[MQTT_MAX_SUBS];
static volatile bool      s_subs_dirty     = false;

static struct will_cfg    s_will;

/* state observer slot - protected by s_subs_mutex */
static mqtt_state_cb_t    s_state_cb           = NULL;
static volatile bool      s_state_cb_last      = false;

/* default-handler pair - protected by s_subs_mutex */
static mqtt_msg_handler_t s_default_handler     = NULL;
static void              *s_default_handler_ctx = NULL;

static volatile bool      s_force_reconnect = false;

/* reconnect throttle with exponential backoff */
static uint32_t           s_next_connect_at_ms = 0;
static uint32_t           s_connect_fail_streak = 0;
/* timestamp when we entered MQTT_STATUS_CONNECTING; used to detect stuck attempts */
static uint32_t           s_connecting_started_at_ms = 0;


static void  mqtt_task(void *arg);
static void  do_connect(void *arg);
static void  do_disconnect(void *arg);
static void  do_publish(void *arg);
static void  do_apply_subscriptions(void *arg);
static struct sub_entry *find_sub_slot_exact(const char *pattern);
static struct sub_entry *find_sub_slot_match(const char *topic);
static struct sub_entry *find_free_sub_slot(void);
static void  invoke_state_cb(bool connected);
static bool  topic_matches(const char *pattern, const char *topic);
static uint32_t compute_backoff_ms(uint32_t streak);

/* ============================================================
 * Topic matcher: MQTT wildcards
 *   '+' matches exactly one level
 *   '#' matches zero or more levels (must be last)
 * ============================================================ */
static bool topic_matches(const char *pattern, const char *topic)
{
    for (;;) {
        if (*pattern == '#') {
            /* "#" matches anything from this point, including empty */
            return true;
        }
        if (*pattern == '/' && *(pattern + 1) == '#' && *topic == '\0') {
            /* "a/#" matches "a" (zero trailing levels per MQTT spec) */
            return true;
        }
        if (*pattern == '+') {
            /* skip exactly one level in the topic */
            while (*topic && *topic != '/') topic++;
            pattern++;
            /* both should now be at '/' or end */
            if (*pattern == '\0' && *topic == '\0') return true;
            if (*pattern == '/' && *topic == '/') { pattern++; topic++; continue; }
            return false;
        }
        if (*pattern == '\0' && *topic == '\0') return true;
        if (*pattern == '\0' || *topic == '\0') return false;
        if (*pattern != *topic) return false;
        pattern++;
        topic++;
    }
}

/* ============================================================
 * Helpers
 *   *_slot_* assume the caller holds s_subs_mutex.
 * ============================================================ */

static struct sub_entry *find_sub_slot_exact(const char *pattern)
{
    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (s_subs[i].used && strcmp(s_subs[i].topic, pattern) == 0) {
            return &s_subs[i];
        }
    }
    return NULL;
}

static struct sub_entry *find_sub_slot_match(const char *topic)
{
    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (s_subs[i].used && !s_subs[i].pending_unsub
            && topic_matches(s_subs[i].topic, topic)) {
            return &s_subs[i];
        }
    }
    return NULL;
}

static struct sub_entry *find_free_sub_slot(void)
{
    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (!s_subs[i].used) return &s_subs[i];
    }
    return NULL;
}

static void invoke_state_cb(bool connected)
{
    /* s_state_cb_last is volatile; transition detection is single-writer-effective
     * because both call sites (tcpip_thread on_connection, mqtt_task link-edge)
     * never both fire for the same logical transition. Mutex around the read
     * keeps the pair (last, cb) consistent. */
    osMutexAcquire(s_subs_mutex, osWaitForever);
    bool last = s_state_cb_last;
    mqtt_state_cb_t cb = s_state_cb;
    if (connected != last) s_state_cb_last = connected;
    osMutexRelease(s_subs_mutex);

    if (connected != last && cb) cb(connected);
}

static uint32_t compute_backoff_ms(uint32_t streak)
{
    uint32_t b = MQTT_BACKOFF_INITIAL_MS;
    /* cap shift to prevent overflow before saturating at the max */
    for (uint32_t i = 0; i < streak && i < 16; i++) {
        b *= 2;
        if (b >= MQTT_BACKOFF_MAX_MS) { b = MQTT_BACKOFF_MAX_MS; break; }
    }
    return b;
}

/* ============================================================
 * lwIP callbacks - ALL run on tcpip_thread.
 * ============================================================ */

static void on_incoming_publish(void *arg, const char *topic, u32_t tot_len)
{
    (void)arg;

    /* Bug 5 fix: if the message won't fit, drop it cleanly (do not dispatch
     * truncated bytes to the handler).
     * Bug G fix: reserve one byte for the null terminator we keep at the
     * tail of mqtt_last_payload so it remains a valid C string for the
     * debugger. Real usable payload capacity is BUF_SIZE - 1. */
    if (tot_len > (u32_t)(MQTT_PAYLOAD_BUF_SIZE - 1)) {
        s_rx_skip = true;
        s_stat_rx_truncated_count++;
        return;
    }
    s_rx_skip = false;

    strncpy(mqtt_last_topic, topic, sizeof(mqtt_last_topic) - 1);
    mqtt_last_topic[sizeof(mqtt_last_topic) - 1] = '\0';
    memset(mqtt_last_payload, 0, sizeof(mqtt_last_payload));
    s_rx_offset = 0;
}

static void on_incoming_data(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    (void)arg;
    if (s_rx_skip) return;

    uint16_t room = (uint16_t)(sizeof(mqtt_last_payload) - 1 - s_rx_offset);
    uint16_t copy = (len < room) ? len : room;
    if (copy > 0) {
        memcpy(&mqtt_last_payload[s_rx_offset], data, copy);
        s_rx_offset = (uint16_t)(s_rx_offset + copy);
        mqtt_last_payload[s_rx_offset] = '\0';
    }

    if (flags & MQTT_DATA_FLAG_LAST) {
        mqtt_last_payload_len = s_rx_offset;
        mqtt_rx_msg_count++;
        s_stat_rx_count++;

        /* Snapshot handler+ctx under mutex; call AFTER release so the handler
         * is free to call back into our API without nested locks. */
        mqtt_msg_handler_t  h         = NULL;
        void               *ctx       = NULL;
        mqtt_msg_handler_t  default_h = NULL;
        void               *default_c = NULL;

        osMutexAcquire(s_subs_mutex, osWaitForever);
        struct sub_entry *e = find_sub_slot_match(mqtt_last_topic);
        if (e) { h = e->handler; ctx = e->user_ctx; }
        else   { default_h = s_default_handler; default_c = s_default_handler_ctx; }
        osMutexRelease(s_subs_mutex);

        if (h)              h(mqtt_last_topic, mqtt_last_payload, mqtt_last_payload_len, ctx);
        else if (default_h) default_h(mqtt_last_topic, mqtt_last_payload, mqtt_last_payload_len, default_c);
    }
}

static void on_pub_done(void *arg, err_t result)
{
    (void)arg;
    if (result == ERR_OK) {
        mqtt_tx_msg_count++;
        s_stat_tx_count++;
    } else {
        mqtt_last_publish_err = (int32_t)result;
        s_stat_last_error     = (int32_t)result;
    }
    osSemaphoreRelease(s_inflight_free);
}

static void on_sub_done(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK) s_stat_last_error = (int32_t)result;
}

static void on_connection(mqtt_client_t *c, void *arg, mqtt_connection_status_t status)
{
    (void)c;
    (void)arg;

    if (status == MQTT_CONNECT_ACCEPTED) {
        mqtt_connect_status   = MQTT_STATUS_CONNECTED;
        s_connect_fail_streak = 0;
        s_stat_connect_count++;

        mqtt_set_inpub_callback(s_client, on_incoming_publish, on_incoming_data, NULL);

        /* Concern 6: mark all live subs as needing replay; the worker will
         * drive do_apply_subscriptions to do the wire work outside any
         * hot path. */
        osMutexAcquire(s_subs_mutex, osWaitForever);
        for (int i = 0; i < MQTT_MAX_SUBS; i++) {
            if (s_subs[i].used && !s_subs[i].pending_unsub) {
                s_subs[i].needs_resubscribe = true;
            }
        }
        osMutexRelease(s_subs_mutex);
        s_subs_dirty = true;

        invoke_state_cb(true);
    } else {
        /* Bug I: unprotected read of s_state_cb_last (a volatile bool, so
         * byte-atomic on Cortex-M7). The race is between this read and the
         * mutex-protected write in invoke_state_cb. Worst case is a stats
         * miscount (disconnect vs connect-fail off by one) on the boundary
         * of a transition; no functional impact. Adding the mutex here would
         * cost a lock+release on the hot connect-fail path for no real gain. */
        if (s_state_cb_last) s_stat_disconnect_count++;
        else                 s_stat_connect_fail_count++;
        s_connect_fail_streak++;
        mqtt_connect_status = MQTT_STATUS_REFUSED_BASE + (uint32_t)status;
        invoke_state_cb(false);
    }
}

/* ============================================================
 * tcpip_thread workers (scheduled via tcpip_callback)
 * ============================================================ */

static void do_disconnect(void *arg)
{
    (void)arg;
    if (s_client) {
        /* Bug BB: mqtt_disconnect() -> tcp_close() puts the PCB in FIN_WAIT_1
         * waiting for the broker's ACK. When the cable is out (or broker is
         * dead) the FIN never reaches anyone, and lwIP retransmits for the
         * full TCP_MAXRTX budget (~5 minutes) before freeing the PCB. During
         * that window a MEMP_NUM_TCP_PCB slot is leaked, and rapid cable
         * cycles exhaust the pool -> mqtt_client_connect returns ERR_MEM
         * (status=201) until cleanup finishes.
         *
         * Fix: send RST and free the PCB immediately via altcp_abort. We do
         * this BEFORE mqtt_disconnect so that mqtt_close sees conn==NULL and
         * skips its tcp_close path. mqtt_disconnect still resets conn_state
         * and clears the pending-request list. */
        if (s_client->conn) {
            altcp_abort(s_client->conn);
            s_client->conn = NULL;
        }
        mqtt_disconnect(s_client);
    }
    /* Bug 4 / Bug T notes:
     *   The behavior of lwIP's mqtt_disconnect regarding pending request
     *   callbacks (on_pub_done, on_sub_done) is version-dependent. Modern
     *   versions call them with an error before clearing the request list;
     *   older versions just free the requests silently. We release the
     *   inflight slot unconditionally to be safe in both cases. If on_pub_done
     *   later fires for the same request, the second osSemaphoreRelease
     *   returns osErrorResource and is harmlessly ignored (semaphore max=1). */
    if (s_inflight_free) osSemaphoreRelease(s_inflight_free);
}

static void do_connect(void *arg)
{
    (void)arg;
    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id   = s_cfg.client_id;
    ci.keep_alive  = s_cfg.keep_alive_s;
    ci.client_user = s_cfg.username;
    ci.client_pass = s_cfg.password;

    if (s_will.used) {
        ci.will_topic  = s_will.topic;
        ci.will_msg    = (const char *)s_will.payload;
        ci.will_qos    = s_will.qos;
        ci.will_retain = s_will.retain ? 1 : 0;
    }

    err_t err = mqtt_client_connect(s_client, &s_broker_addr, s_cfg.broker_port,
                                    on_connection, NULL, &ci);
    if (err == ERR_ISCONN) {
        /* lwIP's client conn_state is not TCP_DISCONNECTED -- typically because
         * a previous connect attempt is still in flight (TCP_CONNECTING) and
         * we somehow got here from a non-CONNECTING status. Tear down the
         * stale attempt and retry once; on second failure, fall through. */
        mqtt_disconnect(s_client);
        err = mqtt_client_connect(s_client, &s_broker_addr, s_cfg.broker_port,
                                  on_connection, NULL, &ci);
    }
    if (err != ERR_OK) {
        mqtt_connect_status   = MQTT_STATUS_CONNECT_ERR_BASE + (uint32_t)(-err);
        s_stat_last_error     = (int32_t)err;
        s_stat_connect_fail_count++;
        s_connect_fail_streak++;
    }
    /* On ERR_OK: lwIP is now in TCP_CONNECTING. We leave mqtt_connect_status
     * at MQTT_STATUS_CONNECTING (set by the worker before scheduling us); the
     * eventual on_connection callback will transition it to CONNECTED or to
     * an error code. */
}

static void do_publish(void *arg)
{
    struct pub_req *r = (struct pub_req *)arg;

    if (s_client && mqtt_client_is_connected(s_client) &&
        s_client->conn != NULL)
    {
        struct tcp_pcb *pcb = s_client->conn;

        /*
         * NEW: check TCP send buffer before calling mqtt_publish().
         * This does not reset or hide any Ethernet error.
         * It only prevents feeding MQTT into lwIP when TCP has no send space.
         */
        if (tcp_sndbuf(pcb) < r->len)
        {
            mqtt_last_publish_err = ERR_MEM;
            s_stat_last_error     = ERR_MEM;

            osSemaphoreRelease(s_inflight_free);
            return;
        }

        err_t err = mqtt_publish(s_client, r->topic, r->payload, r->len,
                                 r->qos, r->retain ? 1 : 0,
                                 on_pub_done, NULL);

        if (err != ERR_OK) {
            mqtt_last_publish_err = (int32_t)err;
            s_stat_last_error     = (int32_t)err;
            osSemaphoreRelease(s_inflight_free);
        }
        /* on success: on_pub_done releases the slot */
    } else {
        osSemaphoreRelease(s_inflight_free);
    }
}
/* Apply all pending sub/unsub changes to the broker.
 * Iterates one entry at a time, holding the mutex only across the snapshot
 * (Concern 3).
 * Bug D: re-checks state under the mutex AFTER the lwIP call, so a user
 *        re-subscribing during our release window does not get clobbered.
 * Bug V: only clears the operation flag on ERR_OK; preserves it on lwIP
 *        failure so the worker retries (and sets s_subs_dirty if any failed
 *        so the next loop tick reschedules us). */
static void do_apply_subscriptions(void *arg)
{
    (void)arg;
    if (!s_client || !mqtt_client_is_connected(s_client)) return;

    bool any_failed = false;

    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        bool    used, needs_resub, pending_unsub;
        char    topic[MQTT_TOPIC_BUF_SIZE];
        uint8_t qos = 0;

        osMutexAcquire(s_subs_mutex, osWaitForever);
        used          = s_subs[i].used;
        needs_resub   = s_subs[i].needs_resubscribe;
        pending_unsub = s_subs[i].pending_unsub;
        if (used) {
            memcpy(topic, s_subs[i].topic, sizeof(topic));
            qos = s_subs[i].qos;
        }
        osMutexRelease(s_subs_mutex);

        if (!used) continue;

        if (pending_unsub) {
            err_t err = mqtt_unsubscribe(s_client, topic, on_sub_done, NULL);
            if (err == ERR_OK) {
                osMutexAcquire(s_subs_mutex, osWaitForever);
                /* Bug D: only clear if the entry is still marked for removal.
                 * If a user re-subscribed in the gap, pending_unsub will be
                 * false and we must leave the slot intact. */
                if (s_subs[i].pending_unsub) {
                    s_subs[i].used = false;
                    s_subs[i].pending_unsub = false;
                    s_subs[i].needs_resubscribe = false;
                    s_subs[i].handler  = NULL;
                    s_subs[i].user_ctx = NULL;
                    memset(s_subs[i].topic, 0, sizeof(s_subs[i].topic));
                }
                osMutexRelease(s_subs_mutex);
            } else {
                s_stat_last_error = (int32_t)err;
                any_failed = true;
            }
        } else if (needs_resub) {
            err_t err = mqtt_subscribe(s_client, topic, qos, on_sub_done, NULL);
            if (err == ERR_OK) {
                osMutexAcquire(s_subs_mutex, osWaitForever);
                /* Only clear if the slot is still ours and still wants
                 * resubscription. If the user unsubscribed meanwhile,
                 * leave pending_unsub for the next pass. */
                if (s_subs[i].used && s_subs[i].needs_resubscribe
                    && !s_subs[i].pending_unsub) {
                    s_subs[i].needs_resubscribe = false;
                }
                osMutexRelease(s_subs_mutex);
            } else {
                s_stat_last_error = (int32_t)err;
                any_failed = true;
            }
        }
    }

    if (any_failed) {
        /* Re-arm the dirty flag so the worker schedules us again next tick.
         * Caps the retry rate to ~MQTT_LOOP_PERIOD_MS. */
        s_subs_dirty = true;
    }
}

/* ============================================================
 * The FreeRTOS worker task
 * ============================================================ */

static void mqtt_task(void *arg)
{
    (void)arg;

    /* 1) Wait for lwIP init + link up. netif_default is set by netif_set_default()
     *    in CubeMX-generated MX_LWIP_Init. */
    while (netif_default == NULL || !netif_is_link_up(netif_default)) {
        osDelay(200);
    }
    osDelay(MQTT_PHY_WARMUP_MS);

    s_client = mqtt_client_new();
    if (s_client == NULL) {
        mqtt_connect_status = MQTT_STATUS_OOM;
        for (;;) osDelay(1000);
    }
    if (!ipaddr_aton(s_cfg.broker_ip, &s_broker_addr)) {
        mqtt_connect_status = MQTT_STATUS_BAD_IP;
        for (;;) osDelay(1000);
    }

    bool link_was_up         = true;
    s_next_connect_at_ms     = osKernelGetTickCount() + MQTT_LINK_SETTLE_MS;

    for (;;) {
        struct netif *nif = netif_default;
        bool link_now     = (nif != NULL) && netif_is_link_up(nif);
        uint32_t now      = osKernelGetTickCount();

        /* --- link edges --- */
        if (link_was_up && !link_now) {
            (void)tcpip_callback(do_disconnect, NULL);
            mqtt_connect_status = MQTT_STATUS_IDLE;
            invoke_state_cb(false); //tells the application that mqtt is disconnected
        } else if (!link_was_up && link_now) {
            s_next_connect_at_ms  = now + MQTT_LINK_SETTLE_MS;
            s_connect_fail_streak = 0;
            mqtt_connect_status   = MQTT_STATUS_IDLE;
        }
        link_was_up = link_now;

        /* --- explicit reconnect request ---
         * Bug R: honor the exponential backoff streak. Otherwise a wedged-
         * publish loop (which sets s_force_reconnect every 10 s on timeout)
         * would reconnect every MQTT_LINK_SETTLE_MS regardless of broker
         * health -- effectively bypassing backoff. */
        if (s_force_reconnect) {
            s_force_reconnect = false;
            (void)tcpip_callback(do_disconnect, NULL);
            mqtt_connect_status  = MQTT_STATUS_IDLE;
            uint32_t bk = compute_backoff_ms(s_connect_fail_streak);
            uint32_t wait = (bk > MQTT_LINK_SETTLE_MS) ? bk : MQTT_LINK_SETTLE_MS;
            s_next_connect_at_ms = now + wait;
        }

        if (!link_now) {
            osDelay(MQTT_LOOP_PERIOD_MS);
            continue;
        }

        if (mqtt_connect_status == MQTT_STATUS_CONNECTED) {
            /* push any pending sub/unsub changes */
            if (s_subs_dirty) {
                s_subs_dirty = false;
                tcpip_callback(do_apply_subscriptions, NULL);
            }

            /* drain one publish per iteration, with timeout on inflight slot */
            struct pub_req r;
            if (osMessageQueueGet(s_pub_queue, &r, NULL, MQTT_LOOP_PERIOD_MS) == osOK) {
                if (osSemaphoreAcquire(s_inflight_free, MQTT_PUB_TIMEOUT_MS) == osOK) {
                    s_inflight = r;
                    err_t cb_err = tcpip_callback(do_publish, &s_inflight);
                    if (cb_err != ERR_OK){
                        osSemaphoreRelease(s_inflight_free);
                        s_stat_pub_timeout_count++;
                    }
                } else {
                    /* Bug 3 fix: inflight slot wedged. Drop this message,
                     * count it, force reconnect to recover lwIP state. */
                    s_stat_pub_timeout_count++;
                    s_force_reconnect = true;
                }
                continue;
            }
        } else if (mqtt_connect_status == MQTT_STATUS_CONNECTING) {
            /* Bug AA: a connect attempt is in flight inside lwIP. Wait for
             * on_connection to transition us out, or force cleanup after a
             * grace period. Without this gate we'd fire mqtt_client_connect
             * again every backoff tick and hit ERR_ISCONN -> status=210. */
            if ((int32_t)(now - s_connecting_started_at_ms) >= MQTT_CONNECT_TIMEOUT_MS) {
                s_stat_connect_fail_count++;
                s_connect_fail_streak++;
                s_force_reconnect = true;
            }
        } else {
            /* Idle / refused / errored -> reconnect when the deadline passes.
             * Bug 1 fix: signed compare so it's wrap-safe AND we don't fire
             * every loop iteration.
             * Bug AA: set status=CONNECTING so we don't re-fire while the
             * attempt is in flight. */
            if ((int32_t)(now - s_next_connect_at_ms) >= 0) {
                mqtt_connect_status        = MQTT_STATUS_CONNECTING;
                s_connecting_started_at_ms = now;
                if (tcpip_callback(do_connect, NULL) != ERR_OK) {
                    mqtt_connect_status = MQTT_STATUS_IDLE;
                    s_connect_fail_streak++;
                    s_stat_connect_fail_count++;
                }
                /* Concern 5: exponential backoff. */
                uint32_t backoff = compute_backoff_ms(s_connect_fail_streak);
                s_next_connect_at_ms = osKernelGetTickCount() + backoff;
            }
        }

        osDelay(MQTT_LOOP_PERIOD_MS);
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void mqtt_init(const mqtt_config_t *cfg)
{
	if (s_initialized || cfg == NULL) return; //S_INITIALISED prevent from initialzing the mqtt again
    s_initialized = true;

    s_cfg = *cfg;

    s_pub_queue = osMessageQueueNew(MQTT_PUB_QUEUE_DEPTH,
									sizeof(struct pub_req),
									NULL); //Queue for publishing request
    s_inflight_free = osSemaphoreNew(1, 1, NULL); //s_inflight_free semaphore is when publishing request is in process

    static const osMutexAttr_t subs_mutex_attr = {
        .name      = "mqttSubs",
        .attr_bits = osMutexPrioInherit,
    };
    s_subs_mutex = osMutexNew(&subs_mutex_attr);

    static const osThreadAttr_t attr = {
        .name       = "mqttTask",
        .stack_size = MQTT_TASK_STACK_BYTES,
        .priority   = osPriorityNormal,
    };
    osThreadNew(mqtt_task, NULL, &attr);
}

//bool mqtt_is_connected(void)
//{
//    return mqtt_connect_status == MQTT_STATUS_CONNECTED;
//}

//void mqtt_set_state_callback(mqtt_state_cb_t cb)
//{
//    /* Bug H: contract is "mqtt_init must complete before this is called."
//     * If caller violates that, silently no-op rather than write unprotected. */
//    if (!s_subs_mutex) return;
//    osMutexAcquire(s_subs_mutex, osWaitForever);
//    s_state_cb = cb;
//    osMutexRelease(s_subs_mutex);
//}

void mqtt_force_reconnect(void)
{
    s_force_reconnect = true;
}

bool mqtt_app_publish(const char *topic, const void *data, size_t len,
                      uint8_t qos, bool retain)
{
    if (s_pub_queue == NULL || topic == NULL || data == NULL) return false;
    if (qos > 2) return false;

    size_t tlen = strlen(topic);
    if (tlen == 0 || tlen >= MQTT_TOPIC_BUF_SIZE) return false;
    if (len > MQTT_PAYLOAD_BUF_SIZE)              return false;

    struct pub_req r;
    memcpy(r.topic, topic, tlen + 1);
    memcpy(r.payload, data, len);
    r.len    = (uint16_t)len;
    r.qos    = qos;
    r.retain = retain;

    if (osMessageQueuePut(s_pub_queue, &r, 0, 0) == osOK) return true;

    /* Bug X: multi-writer counter (any task can be here). Use GCC's
     * atomic builtin so concurrent failures aren't lost. On Cortex-M7
     * this lowers to LDREX/STREX inline. */
    __atomic_fetch_add(&s_stat_tx_drop_count, 1, __ATOMIC_RELAXED);
    return false;
}

bool mqtt_app_publish_string(const char *topic, const char *str)
{
    if (!str) return false;
    return mqtt_app_publish(topic, str, strlen(str), 1, false);
}

//bool mqtt_app_publish_printf(const char *topic, const char *fmt, ...)
//{
//    /* WARNING: 256 bytes of CALLER stack. See header. */
//    char buf[MQTT_PAYLOAD_BUF_SIZE];
//    va_list args;
//    va_start(args, fmt);
//    int n = vsnprintf(buf, sizeof(buf), fmt, args);
//    va_end(args);
//    if (n < 0 || n >= (int)sizeof(buf)) return false;
//    return mqtt_app_publish(topic, buf, (size_t)n, 1, false);
//}

//bool mqtt_app_publish_retained(const char *topic, const void *data, size_t len)
//{
//    return mqtt_app_publish(topic, data, len, 1, true);
//}

bool mqtt_app_subscribe(const char *topic, uint8_t qos,
                        mqtt_msg_handler_t handler, void *user_ctx)
{
    if (!s_initialized || !topic || !handler) return false;
    if (qos > 2) return false;
    size_t tlen = strlen(topic);
    if (tlen == 0 || tlen >= MQTT_TOPIC_BUF_SIZE) return false;

    osMutexAcquire(s_subs_mutex, osWaitForever);
    struct sub_entry *e = find_sub_slot_exact(topic);
    if (!e) e = find_free_sub_slot();
    if (!e) {
        osMutexRelease(s_subs_mutex);
        return false;
    }
    memcpy(e->topic, topic, tlen + 1);
    e->qos               = qos;
    e->handler           = handler;
    e->user_ctx          = user_ctx;
    e->used              = true;
    e->needs_resubscribe = true;
    e->pending_unsub     = false;
    osMutexRelease(s_subs_mutex);

    s_subs_dirty = true;
    return true;
}

bool mqtt_app_unsubscribe(const char *topic)
{
    if (!s_initialized || !topic) return false;

    osMutexAcquire(s_subs_mutex, osWaitForever);
    struct sub_entry *e = find_sub_slot_exact(topic);
    if (e) {
        /* Bug 2 fix: keep the entry, mark for removal. Worker sends
         * UNSUBSCRIBE to the broker then clears the slot. */
        e->pending_unsub     = true;
        e->needs_resubscribe = false;
    }
    osMutexRelease(s_subs_mutex);

    if (e) s_subs_dirty = true;
    /* Bug N: report whether we actually had a subscription to remove.
     * Caller can use this to detect a typo or stale unsubscribe call. */
    return e != NULL;
}

void mqtt_set_default_handler(mqtt_msg_handler_t h, void *user_ctx)
{
    /* Bug 8 fix: mutex-protect the pair so dispatcher sees them as a unit.
     * Bug H: no unprotected fallback - if init isn't done, silently no-op. */
    if (!s_subs_mutex) return;
    osMutexAcquire(s_subs_mutex, osWaitForever);
    s_default_handler     = h;
    s_default_handler_ctx = user_ctx;
    osMutexRelease(s_subs_mutex);
}

bool mqtt_set_will(const char *topic, const void *payload, size_t len,
                   uint8_t qos, bool retain)
{
    /* Bug P: any validation failure preserves the prior will unchanged and
     * returns false so the caller can detect a rejected update.
     * Special case: topic == NULL OR payload == NULL is the documented
     * "clear the will" operation and returns true. */
    if (topic == NULL || payload == NULL) {
        s_will.used = false;
        return true;
    }
    if (qos > 2)                                   return false;
    size_t tlen = strlen(topic);
    if (tlen == 0 || tlen >= sizeof(s_will.topic)) return false;
    if (len > sizeof(s_will.payload))              return false;

    memcpy(s_will.topic, topic, tlen + 1);
    memcpy(s_will.payload, payload, len);
    s_will.len    = len;
    s_will.qos    = qos;
    s_will.retain = retain;
    s_will.used   = true;
    return true;
}

void mqtt_get_stats(mqtt_stats_t *out)
{
    if (!out) return;
    out->connect_count        = s_stat_connect_count;
    out->disconnect_count     = s_stat_disconnect_count;
    out->connect_fail_count   = s_stat_connect_fail_count;
    out->tx_count             = s_stat_tx_count;
    out->rx_count             = s_stat_rx_count;
    out->tx_drop_count        = s_stat_tx_drop_count;
    out->rx_truncated_count   = s_stat_rx_truncated_count;
    out->pub_timeout_count    = s_stat_pub_timeout_count;
    out->last_error           = s_stat_last_error;
}
