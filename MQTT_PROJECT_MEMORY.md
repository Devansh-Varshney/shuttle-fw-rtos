# MQTT Project Memory — Handoff Document

**Purpose:** Drop this entire file into a new Claude chat at the start of any
future session on this project. It contains everything needed to pick up
without re-explaining context.

**Last updated:** End of audit/fix cycle after senior-architect review.

---

## 0. How future-Claude should use this

You are continuing work on an STM32H753 firmware project doing MQTT over
Ethernet with FreeRTOS + lwIP. Treat this file as ground truth for:
- What hardware/software stack we're on
- What's already built and how it's structured
- What design decisions are locked in (don't relitigate unless asked)
- The user's preferences and learning style
- Outstanding work and known limitations

If anything in this file contradicts the live code, **the live code wins**.
Re-read the actual source before making claims; this file is a snapshot.

---

## 1. Hardware & Toolchain

| Item | Value |
|---|---|
| MCU | STM32H753IIT6 (Cortex-M7, single core) |
| Board | Custom (not a Nucleo/Disco) |
| PHY | RMII, link is working — ping is solid |
| IDE | STM32CubeIDE |
| HAL | STM32H7 Cube HAL via CubeMX |
| RTOS | FreeRTOS via CMSIS-RTOS v2 wrapper |
| Network stack | lwIP 2.1.x (Middlewares/Third_Party/LwIP) |
| MQTT | lwIP's raw `lwip/apps/mqtt.h` (no TLS — port 1883) |
| Cache | I-cache + D-cache enabled (`SCB_EnableICache/DCache`) |
| MPU | Configured in `MPU_Config()` for non-cacheable DMA regions |
| Debug output | **Live Expressions only — NO UART printf.** Anything you want visible must go through volatile globals. |

### Static IP

Configured in `LWIP/App/lwip.c` `MX_LWIP_Init`:
- STM32 IP: **192.168.0.123**
- Netmask: 255.255.255.0
- Gateway: 192.168.0.1
- No DHCP

### Broker (mosquitto on laptop)

- Laptop IP: **192.168.0.37** (the user must keep this IP on their Ethernet adapter)
- Port: **1883** (plaintext)
- No auth (`allow_anonymous true`)
- Config file: `C:\mosquitto\mqtt.conf` with `listener 1883 0.0.0.0` + `allow_anonymous true`
- Windows firewall rule opened on TCP 1883

### Client identity & topics

- **Client ID:** `shuttle_1`
- **Subscribe topic (laptop → STM32):** `shuttle/wcs`
- **Publish topic (STM32 → laptop):** `shuttle/telemetry`
- Keep-alive: 60 seconds

---

## 2. User profile (DevanshVarshney)

Embedded developer learning STM32 networking. Treats this project as a
learning exercise plus production-track work.

**Preferences (saved to long-term memory; honor these):**

1. **Teacher-mode explanations.** When asked to explain, walk through *why*
   things are structured the way they are, not just *what* they do.
   Reference: their explicit ask "guide me like teacher."
2. **They usually implement themselves** after you explain the design.
   Exception: for the MQTT module they asked you to write the code outright.
   Default behavior: explain + show snippets, don't `Edit/Write` project files
   unless they say "implement," "apply," "make the changes," etc.
3. **Live Expressions for debugging**, not printf. Everything debuggable
   must be reachable from a global symbol (file-scope, non-`static`).
4. **Wants senior-architect-level rigor.** When you claim code is correct,
   that means *static analysis* — not just signature matching. **Grep for
   name collisions with library headers** (lwIP defines `mqtt_publish` as
   a function and `mqtt_subscribe`/`mqtt_unsubscribe` as macros). The user
   has called you out twice for skipping this check. Don't skip it again.
5. **Direct feedback is welcome.** If their proposed bug is wrong, say so
   and explain. If it's right, fix it. Don't fix non-bugs.
6. **No emojis** in code or output unless explicitly requested.

---

## 3. Project layout (relevant files only)

```
FREERTOS_Ethernet_ RMII_LWIP_PING/
├── Core/
│   ├── Inc/
│   │   ├── main.h
│   │   └── mqtt_task.h         ← public MQTT API header
│   └── Src/
│       ├── main.c              ← calls mqtt_init, has tcp_server/client tasks too
│       ├── mqtt_task.c         ← generic MQTT implementation
│       └── ... (CubeMX-generated)
├── LWIP/App/
│   ├── lwip.h
│   └── lwip.c                  ← MX_LWIP_Init creates tcpip_thread + EthLink thread
├── Middlewares/Third_Party/
│   ├── LwIP/                   ← lwIP 2.1.x source
│   └── FreeRTOS/               ← FreeRTOS + CMSIS-RTOS v2
└── MQTT_PROJECT_MEMORY.md      ← THIS FILE
```

### What main.c contains (post-MQTT integration)

- `defaultTask` (CubeMX-generated, runs `StartDefaultTask`)
  - Calls `MX_LWIP_Init()` after a 1 s PHY warm-up delay
  - Creates `tcpServer` and `tcpClient` tasks (legacy, kept)
  - Calls `mqtt_app_subscribe("shuttle/wcs", 1, on_cmd, NULL)` once at startup
  - Loops publishing `"hello from generic API"` every 5 s via `mqtt_app_publish_string`
- File-scope test handler `on_cmd` that increments `mqtt_cmd_recv_count`
  and `mqtt_cmd_last_len` (both volatile globals, visible in Live Expressions)
- `mqtt_init(&mqtt_cfg)` called before `osKernelStart` with config struct

### What mqtt_task.h exposes (public API)

```c
/* Status constants */
#define MQTT_STATUS_IDLE              0u
#define MQTT_STATUS_CONNECTED         1u
#define MQTT_STATUS_BAD_IP            98u
#define MQTT_STATUS_OOM               99u
#define MQTT_STATUS_REFUSED_BASE      100u  /* + MQTT_CONNECT_REFUSED_* */
#define MQTT_STATUS_DISCONNECTED      356u  /* 100 + 256 */
#define MQTT_STATUS_CONNECT_ERR_BASE  200u  /* + (-err_t) */

/* Types */
typedef struct { broker_ip, broker_port, client_id, keep_alive_s,
                 username, password } mqtt_config_t;
typedef void (*mqtt_msg_handler_t)(topic, payload, len, user_ctx);
typedef void (*mqtt_state_cb_t)(bool connected);
typedef struct { connect_count, disconnect_count, connect_fail_count,
                 tx_count, rx_count, tx_drop_count, rx_truncated_count,
                 pub_timeout_count, last_error } mqtt_stats_t;

/* Lifecycle */
void mqtt_init(const mqtt_config_t *cfg);
bool mqtt_is_connected(void);
void mqtt_set_state_callback(mqtt_state_cb_t cb);
void mqtt_force_reconnect(void);

/* Publishing (note: mqtt_app_* prefix to avoid collision with lwIP) */
bool mqtt_app_publish(topic, data, len, qos, retain);
bool mqtt_app_publish_string(topic, str);
bool mqtt_app_publish_printf(topic, fmt, ...);
bool mqtt_app_publish_retained(topic, data, len);

/* Subscribing (also mqtt_app_*) */
bool mqtt_app_subscribe(topic, qos, handler, user_ctx);
bool mqtt_app_unsubscribe(topic);  /* returns true only if found */
void mqtt_set_default_handler(h, user_ctx);

/* LWT */
bool mqtt_set_will(topic, payload, len, qos, retain);  /* false on rejection */

/* Diagnostics */
void mqtt_get_stats(mqtt_stats_t *out);
```

---

## 4. Architecture (design decisions — locked in)

### Three constraints that drive the entire design

1. **Only `tcpip_thread` may call lwIP.** Everything else marshals via
   `tcpip_callback(fn, arg)`.
2. **`tcpip_callback` is asynchronous** — the pointer you pass must remain
   valid until the callback runs. Stack-locals don't work.
3. **Multiple producers race.** Handle thread safety at every public API.

### Pattern: producer/consumer through queues

- **Outgoing publishes:** application task → `mqtt_app_publish` → memcpy into
  FreeRTOS message queue (`s_pub_queue`) → mqtt_task drains → copies to
  single `s_inflight` slot (guarded by binary semaphore `s_inflight_free`) →
  `tcpip_callback(do_publish, &s_inflight)` → tcpip_thread runs `mqtt_publish`.
- **Incoming messages:** lwIP fires `on_incoming_publish` then 1+ calls to
  `on_incoming_data` → on LAST flag, reassembled payload is in
  `mqtt_last_payload` → dispatcher looks up topic in `s_subs` (with wildcard
  matching) → calls user's handler **on tcpip_thread** (handler must be fast).
- **Subscriptions:** application task → `mqtt_app_subscribe` → adds to
  `s_subs[]` table under mutex, sets `needs_resubscribe` flag → sets
  `s_subs_dirty` → mqtt_task notices, schedules
  `tcpip_callback(do_apply_subscriptions, NULL)` → worker iterates table
  one entry at a time (snapshot pattern, mutex released across lwIP calls).
- **Unsubscriptions:** same path, uses `pending_unsub` flag. Worker sends
  `mqtt_unsubscribe` to broker then clears the slot (under re-acquired mutex,
  with a re-check that prevents clobbering a fast user re-subscribe).

### Pattern: link-aware reconnect

- **Link state:** `netif_default` (not `gnetif` — portable). Poll every
  `MQTT_LOOP_PERIOD_MS` (100 ms) in the worker.
- **Link-down edge** → `do_disconnect` scheduled → semaphore released to
  unwedge any in-flight publish → state = IDLE.
- **Link-up edge** → reset backoff streak, settle timer.
- **Reconnect throttle:** `s_next_connect_at_ms` with **signed compare**
  `(int32_t)(now - target) >= 0` (wrap-safe). After every connect attempt,
  push `s_next_connect_at_ms = now + compute_backoff_ms(fail_streak)`.
- **Exponential backoff:** 1s → 2s → 4s → 8s → 16s → 32s → 60s (cap).
  Resets on successful CONNACK or link-up edge.
- **Force-reconnect:** sets `s_force_reconnect = true`. Worker sees it,
  schedules `do_disconnect`, **applies the same backoff** (so a
  publish-timeout loop can't bypass throttling).

### Pattern: in-flight slot management

- `s_inflight` is a single static `struct pub_req`.
- `s_inflight_free` is a binary semaphore (max=1, init=1).
- Acquire semaphore with **timeout** (`MQTT_PUB_TIMEOUT_MS = 10 s`). On
  timeout: drop the message, increment `s_stat_pub_timeout_count`, set
  `s_force_reconnect`. The next force-reconnect path releases the semaphore.
- On disconnect, `do_disconnect` unconditionally releases the semaphore
  (lwIP behavior re: pending request callbacks is version-dependent;
  the release is safe regardless because the second release just returns
  `osErrorResource` and is ignored).

### Pattern: wildcard topic matching

Supports MQTT spec wildcards:
- `+` matches exactly one topic level
- `#` matches zero or more levels (must be last in pattern)
- `a/+/c` matches `a/b/c`; `a/#` matches `a`, `a/b`, `a/b/c/d`

Exact-match (`strcmp`) is still used by `mqtt_app_subscribe`/`_unsubscribe`
to find an existing slot (you don't pattern-match patterns).

---

## 5. Threading model — quick reference

| Thread | Created by | What runs there |
|---|---|---|
| `IDLE` | FreeRTOS auto | WFI when nothing else is ready |
| `Tmr Svc` | FreeRTOS auto | software timer callbacks (if any) |
| `defaultTask` | CubeMX, in main.c | `MX_LWIP_Init`, spawns tcp tasks, idle loop |
| `tcpServer`, `tcpClient` | from `StartDefaultTask` | legacy TCP code (not touched) |
| `EthLink` | inside `MX_LWIP_Init` (lwip.c) | PHY link state polling |
| `tcpip_thread` | inside `tcpip_init` | **the** lwIP thread; only thread allowed to touch lwIP state directly. All our lwIP callbacks (on_connection, on_pub_done, on_incoming_*) run here. |
| `mqttTask` | inside `mqtt_init` | our worker: link-state monitor, publish-queue drainer, reconnect throttler |

**Memory model:** flat, no MMU isolation between tasks. Each task gets its
own stack from FreeRTOS heap. Globals/statics shared.

---

## 6. Limits & sizing (file-scope `#define`s in mqtt_task.c)

| Constant | Value | Notes |
|---|---|---|
| `MQTT_MAX_SUBS` | 16 | Subscription table size |
| `MQTT_PUB_QUEUE_DEPTH` | 8 | Outgoing publish backlog |
| `MQTT_TOPIC_BUF_SIZE` | 128 | Topic max (was 64, bumped per audit) |
| `MQTT_PAYLOAD_BUF_SIZE` | 256 | Payload max; `_Static_assert` guards <=512 |
| `MQTT_WILL_PAYLOAD_SIZE` | 64 | Will message max |
| `MQTT_TASK_STACK_BYTES` | 4096 | mqttTask stack |
| `MQTT_LOOP_PERIOD_MS` | 100 | Worker tick |
| `MQTT_LINK_SETTLE_MS` | 500 | Delay after link-up before first connect |
| `MQTT_PHY_WARMUP_MS` | 500 | Delay at boot after netif is up |
| `MQTT_BACKOFF_INITIAL_MS` | 1000 | Backoff start |
| `MQTT_BACKOFF_MAX_MS` | 60000 | Backoff cap |
| `MQTT_PUB_TIMEOUT_MS` | 10000 | In-flight slot watchdog |

---

## 7. Bugs found & fixed (audit history)

These are all **resolved** in the current code. Listed so future-Claude
doesn't re-introduce them.

### First-pass bugs (cascade fixes)
- **Cascade bug:** stray `{` in mqtt_task.h made every following file's
  top-level code parse as nested functions → 27 errors. Fixed by removing.
- **CMSIS-RTOS v1/v2 mismatch:** `osThreadDef`/`osThreadCreate` are v1
  macros. Project is v2. Use `osThreadNew(fn, NULL, &attr)`.
- **Task signature:** v2 entry is `void task(void *arg)` (no `const`).
- **Name collision with lwIP:** `mqtt_publish` (lwIP function) +
  `mqtt_subscribe`/`mqtt_unsubscribe` (lwIP macros). Renamed our public
  API to `mqtt_app_*` prefix for these six functions.

### Cable-cycle recovery bug
- Cable cycle → MQTT didn't recover for ~60 s (waited on keep-alive).
- Fix: actively watch `netif_is_link_up`, tear down + reconnect on edges.

### Senior-review audit pass

Bugs the user found and we fixed:
- **Bug 1** — Reconnect throttle wraparound (unsigned underflow). Fix:
  signed compare, separate `s_next_connect_at_ms` variable.
- **Bug 2** — `mqtt_app_unsubscribe` never told broker. Fix: `pending_unsub`
  flag, `do_apply_subscriptions` sends UNSUBSCRIBE.
- **Bug 3** — `osSemaphoreAcquire(osWaitForever)` could deadlock. Fix:
  `MQTT_PUB_TIMEOUT_MS` timeout, force-reconnect on miss.
- **Bug 4** — Force-reconnect leaked in-flight semaphore. Fix:
  `do_disconnect` unconditionally releases.
- **Bug 5** — Silent truncation of inbound messages. Fix: `s_rx_skip` flag
  set in `on_incoming_publish` when `tot_len > BUF - 1`.
- **Bug 6** — No MQTT wildcards. Fix: `topic_matches()` with `+` and `#`.
- **Bug 7** — RX globals racy from app code (documented as debug-only).
- **Bug 8** — Default handler torn writes. Fix: mutex around the pair.
- **Bug 9** — `mqtt_init` race window. Fix: set `s_initialized = true`
  before doing setup work.

Fifth pass (tcp_client_task socket leak):
- **Bug CC** — `tcp_client_task` in main.c only called `closesocket(sock)`
  on the success branch of `connect()`. On the failure branch (which fires
  every 3 s when the target server at 192.168.0.124 is unreachable, or when
  cable is unplugged), the socket leaked one MEMP_NUM_TCP_PCB slot.
  This is what was actually exhausting the pool, NOT the MQTT cleanup.
  Fix: add `closesocket(sock)` to the else branch in tcp_client_task.

Fourth pass (rapid-cable-cycle PCB leak):
- **Bug BB** — Pulling and replugging Ethernet quickly leaks TCP PCBs in
  FIN_WAIT_1, exhausting `MEMP_NUM_TCP_PCB` (default 5) so subsequent
  `mqtt_client_connect` calls return `ERR_MEM` → `status = 201`. Recovery
  takes ~5 minutes (TCP retransmit budget on the leaked FIN).
  Fix: in `do_disconnect`, call `altcp_abort(s_client->conn)` BEFORE
  `mqtt_disconnect`. This sends RST and frees the memp slot immediately
  instead of waiting for TCP cleanup. Requires `#include
  "lwip/apps/mqtt_priv.h"` (private header that exposes `client->conn`).
  Also bumped `MEMP_NUM_TCP_PCB` to 10 in lwipopts.h as defense-in-depth.

Third pass (broker-down reconnect bug):
- **Bug AA** — Repeatedly calling `mqtt_client_connect` while a previous
  attempt is in lwIP's `TCP_CONNECTING` state hits `ERR_ISCONN`, sticking
  `mqtt_connect_status` at 210. Symptom: kill broker, see status=210, restart
  broker, status stays 210 and never recovers (only cable cycle fixes it).
  Fix: add `MQTT_STATUS_CONNECTING` state. Worker sets it before scheduling
  `do_connect` and gates retries on it. Timeout (30 s) recovers from stuck
  CONNECTING via `s_force_reconnect`. `do_connect` also defensively calls
  `mqtt_disconnect` + retry once if it ever sees `ERR_ISCONN`.

Second audit pass:
- **Bug D** — `do_apply_subscriptions` race between lwIP call and
  re-acquire-mutex-and-clear. Fix: re-check `pending_unsub` and
  `needs_resubscribe` under re-acquired mutex.
- **Bug G** — Off-by-one on `tot_len` check (255 usable, not 256).
- **Bug H** — Removed racy `if (!s_subs_mutex)` unprotected-write fallback.
  Contract: caller waits for `mqtt_init` to return.
- **Bug I** — Stats miscount in `on_connection`'s `s_state_cb_last` read.
  Documented as acceptable.
- **Bug M** — `_Static_assert(MQTT_PAYLOAD_BUF_SIZE <= 512)`.
- **Bug N** — `mqtt_app_unsubscribe` now returns `e != NULL`.
- **Bug P** — `mqtt_set_will` returns `bool`, preserves prior will on
  rejection.
- **Bug R** — Force-reconnect applies exponential backoff (was bypassing).
- **Bug T** — Documented lwIP `mqtt_disconnect` semantics assumption.
- **Bug V** — `mqtt_subscribe`/`mqtt_unsubscribe` return value checked.
  Flag preserved on failure, `s_subs_dirty` re-armed to retry.
- **Bug X** — `s_stat_tx_drop_count` is multi-writer; use
  `__atomic_fetch_add` (compiles to LDREX/STREX inline on M7).

---

## 8. Concerns acknowledged but NOT fixed (intentional)

| Concern | Why kept |
|---|---|
| Handler runs on tcpip_thread | Architectural tradeoff. Refactor to worker dispatch is a future enhancement. Documented in header. |
| 256-byte stack buffer in `mqtt_app_publish_printf` | Documented; tasks with small stacks must use `mqtt_app_publish` directly. `_Static_assert` prevents future bloat. |
| No TLS | Use `altcp_mqtt` + mbedTLS as a future migration. |
| No per-publish delivery confirmation callback | Future enhancement. Stats counters suffice today. |
| Stats miscount on connect/disconnect boundary | Documented; functional impact zero. |
| `mqtt_app_publish` from ISR | osMessageQueuePut is ISR-safe but the surrounding `memcpy`/`strlen`/stack frame make it a bad idea. Documented. |

---

## 9. Live Expressions to watch while debugging

### Status / counters
- `mqtt_connect_status` — `1` = connected, `0` = idle, `98` = bad IP, `99` = OOM, `100..107` = broker refused, `200+` = sync connect error
- `mqtt_rx_msg_count`, `mqtt_tx_msg_count` — should tick during traffic
- `mqtt_last_publish_err` — recent err_t on failed publish

### Last message (debug-only — do NOT consume from app code)
- `mqtt_last_topic` — topic of most recent message
- `mqtt_last_payload` — payload (NUL-terminated)
- `mqtt_last_payload_len` — actual length

### Test subscription (added in main.c)
- `mqtt_cmd_recv_count` — increments per message on `shuttle/wcs`
- `mqtt_cmd_last_len` — length of latest

### Statistics (use `mqtt_get_stats()` from code; not directly visible)
- connect_count, disconnect_count, connect_fail_count
- tx_count, rx_count, tx_drop_count
- rx_truncated_count, pub_timeout_count
- last_error

---

## 10. Mosquitto test commands (Windows laptop)

```powershell
# Start broker (Terminal 1)
& "C:\Program Files\mosquitto\mosquitto.exe" -v -c C:\mosquitto\mqtt.conf

# Subscribe to STM32 telemetry (Terminal 2)
mosquitto_sub -h 192.168.0.37 -t "shuttle/telemetry" -v

# Publish a command (Terminal 3)
mosquitto_pub -h 192.168.0.37 -t "shuttle/wcs" -m "hello"
```

### Tests done / to do
- ✅ Basic ping STM32 ↔ laptop
- ✅ STM32 connects, subscribes, publishes "hello from generic API"
- ✅ Cable unplug/replug — should auto-recover with the link-watch fix
- ⬜ Wildcard: subscribe `shuttle/+`, publish to `shuttle/anything`,
  verify handler fires
- ⬜ Exponential backoff: kill broker, watch s_connect_fail_streak in
  Live Expressions, verify intervals 1, 2, 4, 8, 16, 32, 60 s
- ⬜ `mqtt_app_unsubscribe`: verify returns false on non-subscribed topic
- ⬜ `mqtt_set_will`: verify returns false on invalid qos (e.g., qos=5)
- ⬜ Truncation: `mosquitto_pub -t shuttle/wcs -f <300byte_file>`, verify
  `mqtt_get_stats().rx_truncated_count` increments and handler does NOT fire

---

## 11. Glossary of internals

| Symbol | What it is |
|---|---|
| `s_initialized` | Set early in `mqtt_init`; guards against double-init races |
| `s_cfg` | Copy of the user's config struct (strings NOT copied — must outlive subsystem) |
| `s_client` | lwIP MQTT client object (from `mqtt_client_new`) |
| `s_broker_addr` | Parsed broker IP |
| `s_rx_offset`, `s_rx_skip` | Inbound assembly cursor + skip flag for oversize msgs |
| `s_pub_queue` | FreeRTOS queue of outgoing `pub_req` items |
| `s_inflight` | Single static `pub_req` handed to tcpip_thread |
| `s_inflight_free` | Binary semaphore guarding `s_inflight` |
| `s_subs[16]` | Subscription table; each entry has `used`, `needs_resubscribe`, `pending_unsub`, topic, qos, handler, user_ctx |
| `s_subs_mutex` | Priority-inheriting mutex protecting `s_subs`, `s_state_cb*`, `s_default_handler*` |
| `s_subs_dirty` | Volatile flag; worker checks it to schedule `do_apply_subscriptions` |
| `s_will` | Last Will config (topic, payload, qos, retain) |
| `s_state_cb`, `s_state_cb_last` | Connection-state observer + last reported state (edge detection) |
| `s_default_handler`, `s_default_handler_ctx` | Catch-all for unmatched topics |
| `s_force_reconnect` | User-triggered reconnect flag |
| `s_next_connect_at_ms` | Reconnect deadline (signed-compare wrap-safe) |
| `s_connect_fail_streak` | Exponential backoff counter |
| `s_stat_*` | Statistics counters (mostly single-writer; `tx_drop` uses `__atomic_fetch_add`) |

### tcpip_thread workers (scheduled via `tcpip_callback`)
- `do_connect` — calls `mqtt_client_connect`
- `do_disconnect` — calls `mqtt_disconnect` + releases `s_inflight_free`
- `do_publish` — calls `mqtt_publish`
- `do_apply_subscriptions` — drains the dirty subscription table

### lwIP callbacks (all run on tcpip_thread)
- `on_incoming_publish` — start of inbound message; size check + topic snapshot
- `on_incoming_data` — payload chunks; on LAST flag, dispatch to handler
- `on_pub_done` — PUBACK arrived (or disconnect tore down request); release semaphore
- `on_sub_done` — SUBACK / UNSUBACK arrived
- `on_connection` — CONNACK or DISCONNECTED; sets status, replays subs on success

---

## 12. Open work / not yet done

Things the user might want to tackle next:

1. **Stats publish task** — a low-priority task that calls `mqtt_get_stats`
   every 60 s and publishes a JSON summary via `mqtt_app_publish_printf`
   on something like `shuttle/diagnostics`. Use QoS 0 to avoid the
   timeout-loop interaction with regular telemetry.
2. **Real command dispatch** — replace the test `on_cmd` handler with code
   that pushes commands to a worker task queue (handler must return fast).
3. **Retained "online" status with LWT** — publish `mqtt_app_publish_retained
   ("shuttle/status", "online", 6, ...)` after connect; set will to publish
   `"offline"` retained on disconnect. Gives the laptop a reliable view.
4. **TLS** — port to `altcp_mqtt` + mbedTLS. Significant work; deferred.
5. **Connection telemetry** — expose `s_connect_fail_streak` in stats.
6. **Wildcard matcher test suite** — host-side unit tests for `topic_matches`.

---

## 13. Things the user has been burned by — handle with care

- **Misclassifying name collisions as "static-analysis clean."** Always
  grep library headers for the literal names you plan to declare.
- **Skipping macro-vs-function distinction.** `mqtt_subscribe` looks like
  a function in lwIP; it's a macro. Test by attempting to use the name
  with a different argument count.
- **Claiming code works without testing the throttle/backoff logic.** The
  user found two real bugs there (Bug 1, Bug R) in successive reviews.
- **Treating `osWaitForever` as "fine because nothing ever takes forever."**
  Add a timeout, even on internal sync primitives.
- **Forgetting that the user reads via Live Expressions, not printf.** Any
  debug data must be reachable through a non-static global symbol.

---

## 14. Conversation snippets worth preserving

### The Three Constraints (memorize these — they drive design choices)

1. **Only `tcpip_thread` can call lwIP** → `tcpip_callback` for marshalling.
2. **`tcpip_callback` is async** → pointer must outlive the call → no stack-locals.
3. **Multiple producers race** → queue / mutex / atomics.

### The Init-Function + Long-Running-Task Pattern

Everything in this codebase follows this shape:
- An init function (one-shot, returns): `MX_LWIP_Init`, `mqtt_init`,
  `MX_GPIO_Init`, `SystemClock_Config`
- A long-running task (infinite for(;;), never returns): `StartDefaultTask`,
  `tcpip_thread`, `mqtt_task`, `EthLink`

The init creates the task; the task does the work forever.

### Task = Thread

CMSIS-RTOS v2 `osThreadNew` is a wrapper over FreeRTOS `xTaskCreate`. Same
object (TCB + stack). Different vocabulary. `defaultTask` is **not special**
— it's just `osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes)`.

---

## End of memory document

Save this file. Hand it back at the start of any future session and we
resume without reloading context. Anything contradicted by the live code
should be treated as stale — re-check the source.
