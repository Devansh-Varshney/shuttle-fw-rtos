#!/usr/bin/env python3
"""
mqtt_echo_test.py

Round-trip echo test for STM32 MQTT.

Each message uses the exact JSON shape:
    {"x_pick":<N>,"y_pick":2,"x_place":156,"y_place":1}

where N is a monotonically increasing counter (the per-message unique
identifier). The STM32 echoes the payload byte-for-byte back to the
response topic. The Pi verifies:
  - the echo arrived,
  - the echo matches what we sent,
  - the order is preserved,
  - the round-trip time is reasonable.

Required STM32-side handler (add this in your firmware):

    static void on_test_req(const char *topic, const void *payload,
                            size_t len, void *user_ctx) {
        (void)topic; (void)user_ctx;
        mqtt_app_publish("test/resp", payload, len, 1, false);
    }

    void echo_test_init(void) {
        mqtt_app_subscribe("test/req", 1, on_test_req, NULL);
    }

Call echo_test_init() after mqtt_init() in main.c.

Usage:
    python3 mqtt_echo_test.py --broker 192.168.0.37 --count 1000 --interval-ms 1000
    python3 mqtt_echo_test.py --broker 192.168.0.37 --duration 3600 --interval-ms 100
    python3 mqtt_echo_test.py --broker 192.168.0.37 --interval-ms 500
"""

import argparse
import csv
import json
import os
import queue
import signal
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt


# =====================================================================
# Defaults
# =====================================================================

DEFAULT_BROKER       = "192.168.0.37"
DEFAULT_PORT         = 1883
DEFAULT_TX_TOPIC     = "shuttle/wcs"
DEFAULT_RX_TOPIC     = "shuttle/forwaded"
DEFAULT_INTERVAL_MS  = 1000
RESPONSE_TIMEOUT_S   = 5.0
SUMMARY_INTERVAL_S   = 10.0

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR    = SCRIPT_DIR / "logs"

# Fixed fields that never change between messages
FIXED_Y_PICK  = 2
FIXED_X_PLACE = 156
FIXED_Y_PLACE = 1

# The "unique id" field — this one increments per message
COUNTER_FIELD = "x_pick"


# =====================================================================
# Terminal output helpers
# =====================================================================

class Term:
    RESET   = "\033[0m"
    BOLD    = "\033[1m"
    DIM     = "\033[2m"
    RED     = "\033[31m"
    GREEN   = "\033[32m"
    YELLOW  = "\033[33m"
    BLUE    = "\033[34m"
    CYAN    = "\033[36m"
    GREY    = "\033[90m"

    def __init__(self):
        self.use_color = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
        self.lock = threading.Lock()

    def _c(self, color, text):
        return f"{color}{text}{self.RESET}" if self.use_color else text

    def _ts(self):
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    def info(self, msg):
        with self.lock:
            print(f"{self._c(self.GREY, self._ts())}  "
                  f"{self._c(self.YELLOW, '*')}  {msg}", flush=True)

    def ok(self, msg):
        with self.lock:
            print(f"{self._c(self.GREY, self._ts())}  "
                  f"{self._c(self.GREEN, '✓')}  {msg}", flush=True)

    def err(self, msg):
        with self.lock:
            print(f"{self._c(self.GREY, self._ts())}  "
                  f"{self._c(self.RED, '✗')}  {self._c(self.RED, msg)}", flush=True)

    def sent(self, counter, payload):
        if not self.use_color:
            with self.lock:
                print(f"{self._ts()}  --> {COUNTER_FIELD}={counter}  {payload}", flush=True)
            return
        with self.lock:
            print(f"{self._c(self.GREY, self._ts())}  "
                  f"{self._c(self.BLUE, '-->')} "
                  f"{COUNTER_FIELD}={counter:<5}  {payload}", flush=True)

    def echo_ok(self, counter, rtt_ms):
        with self.lock:
            print(f"{self._c(self.GREY, self._ts())}  "
                  f"{self._c(self.GREEN, '<--')} "
                  f"{COUNTER_FIELD}={counter:<5}  rtt={rtt_ms:.1f} ms  "
                  f"{self._c(self.GREEN, 'MATCH')}", flush=True)

    def echo_bad(self, counter, reason):
        with self.lock:
            print(f"{self._c(self.GREY, self._ts())}  "
                  f"{self._c(self.RED, '<--')} "
                  f"{COUNTER_FIELD}={counter}  "
                  f"{self._c(self.RED, 'MISMATCH')}  {reason}", flush=True)

    def banner(self, msg):
        line = "=" * 78
        with self.lock:
            print(self._c(self.CYAN, line))
            print(self._c(self.BOLD, msg))
            print(self._c(self.CYAN, line))

    def summary(self, line):
        with self.lock:
            print(self._c(self.BOLD, line), flush=True)


term = Term()


# =====================================================================
# Test state
# =====================================================================

class TestState:
    def __init__(self):
        self.lock         = threading.Lock()
        self.in_flight    = {}      # counter -> (sent_mono, sent_dict, sent_str)
        self.sent         = 0
        self.echoed_ok    = 0
        self.echoed_bad   = 0
        self.lost         = 0
        self.unexpected   = 0
        self.rtt_count    = 0
        self.rtt_sum_ms   = 0.0
        self.rtt_min_ms   = float("inf")
        self.rtt_max_ms   = 0.0
        self.last_seen    = 0
        self.out_of_order = 0


state = TestState()
shutdown_evt = threading.Event()
csv_writer   = None
csv_file     = None


# =====================================================================
# CSV logging
# =====================================================================

def open_csv():
    global csv_writer, csv_file
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = LOG_DIR / f"echo_test_{stamp}.csv"
    csv_file = open(path, "w", newline="", buffering=1)
    csv_writer = csv.writer(csv_file)
    csv_writer.writerow([
        "event",
        COUNTER_FIELD,        # the counter value as the identifier
        "sent_iso",
        "echo_iso",
        "rtt_ms",
        "sent_payload",
        "echo_payload",
        "note",
    ])
    return path


def csv_log(event, counter, sent_iso, echo_iso, rtt_ms,
            sent_payload, echo_payload, note):
    if csv_writer is None:
        return
    try:
        csv_writer.writerow([
            event,
            counter if counter is not None else "",
            sent_iso or "",
            echo_iso or "",
            f"{rtt_ms:.3f}" if rtt_ms is not None else "",
            sent_payload or "",
            echo_payload or "",
            note or "",
        ])
    except Exception:
        pass


def close_csv():
    global csv_file
    if csv_file:
        try: csv_file.close()
        except Exception: pass
        csv_file = None


# =====================================================================
# MQTT callbacks
# =====================================================================

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        term.ok("Connected to broker (rc=0)")
        client.subscribe(userdata["rx_topic"], qos=1)
        term.info(f"Subscribed to '{userdata['rx_topic']}'")
    else:
        term.err(f"Connect failed rc={rc}")


def on_disconnect(client, userdata, rc, properties=None):
    term.err(f"Disconnected (rc={rc}); paho will auto-reconnect")


def on_message(client, userdata, msg):
    recv_mono = time.monotonic()
    recv_ts   = datetime.now()
    raw       = msg.payload.decode("utf-8", errors="replace")

    try:
        echoed = json.loads(raw)
        counter = int(echoed[COUNTER_FIELD])
    except (ValueError, KeyError, json.JSONDecodeError) as e:
        with state.lock:
            state.unexpected += 1
        term.err(f"Malformed echo (no '{COUNTER_FIELD}'): {raw[:80]!r}")
        csv_log("UNEXPECTED", None, "", recv_ts.isoformat(timespec="seconds"),
                None, None, raw, f"parse error: {e}")
        return

    # Look up in-flight by counter value
    with state.lock:
        entry = state.in_flight.pop(counter, None)

    if entry is None:
        with state.lock:
            state.unexpected += 1
        term.echo_bad(counter, "no in-flight match (late, dup, or unsent)")
        csv_log("UNEXPECTED", counter, "", recv_ts.isoformat(timespec="seconds"),
                None, None, raw, "no in-flight record")
        return

    sent_mono, sent_dict, sent_str = entry
    rtt_ms = (recv_mono - sent_mono) * 1000.0
    match = (echoed == sent_dict)

    with state.lock:
        state.rtt_count += 1
        state.rtt_sum_ms += rtt_ms
        state.rtt_min_ms = min(state.rtt_min_ms, rtt_ms)
        state.rtt_max_ms = max(state.rtt_max_ms, rtt_ms)

        if counter < state.last_seen:
            state.out_of_order += 1
        state.last_seen = max(state.last_seen, counter)

        if match:
            state.echoed_ok += 1
        else:
            state.echoed_bad += 1

    sent_iso = datetime.fromtimestamp(
        time.time() - (time.monotonic() - sent_mono)
    ).isoformat(timespec="seconds")

    if match:
        term.echo_ok(counter, rtt_ms)
        csv_log("ECHO_OK", counter, sent_iso,
                recv_ts.isoformat(timespec="seconds"),
                rtt_ms, sent_str, raw, "")
    else:
        reason = f"sent={sent_dict} got={echoed}"
        term.echo_bad(counter, reason)
        csv_log("ECHO_BAD", counter, sent_iso,
                recv_ts.isoformat(timespec="seconds"),
                rtt_ms, sent_str, raw, reason)


# =====================================================================
# Publisher thread
# =====================================================================

def publisher_thread(client, args):
    tx_topic     = args.tx_topic
    interval_ms  = args.interval_ms
    target_count = args.count
    deadline     = (time.monotonic() + args.duration) if args.duration > 0 else None
    counter      = 0

    while not shutdown_evt.is_set():
        if target_count > 0 and counter >= target_count:
            term.info(f"Reached --count target ({target_count}); stopping publisher")
            break
        if deadline is not None and time.monotonic() >= deadline:
            term.info(f"Reached --duration ({args.duration}s); stopping publisher")
            break

        counter += 1
        payload = {
            COUNTER_FIELD: counter,
            "y_pick":  FIXED_Y_PICK,
            "x_place": FIXED_X_PLACE,
            "y_place": FIXED_Y_PLACE,
        }
        payload_str = json.dumps(payload, separators=(",", ":"))

        with state.lock:
            state.sent += 1
            state.in_flight[counter] = (time.monotonic(), payload, payload_str)

        try:
            info = client.publish(tx_topic, payload_str, qos=1, retain=False)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                term.err(f"Publish {COUNTER_FIELD}={counter} got rc={info.rc}")
        except Exception as e:
            term.err(f"Publish {COUNTER_FIELD}={counter} exception: {e}")

        term.sent(counter, payload_str)
        csv_log("SENT", counter,
                datetime.now().isoformat(timespec="seconds"),
                "", None, payload_str, "", "")

        slept = 0
        while slept < interval_ms and not shutdown_evt.is_set():
            chunk = min(50, interval_ms - slept)
            time.sleep(chunk / 1000.0)
            slept += chunk

    if not shutdown_evt.is_set():
        grace = RESPONSE_TIMEOUT_S + 2.0
        term.info(f"Waiting {grace:.1f}s for stragglers…")
        shutdown_evt.wait(timeout=grace)
    shutdown_evt.set()


# =====================================================================
# Timeout sweeper
# =====================================================================

def timeout_sweeper():
    while not shutdown_evt.is_set():
        shutdown_evt.wait(timeout=1.0)
        now = time.monotonic()
        timed_out = []
        with state.lock:
            for counter, (sent_mono, sent_dict, sent_str) in list(state.in_flight.items()):
                if now - sent_mono > RESPONSE_TIMEOUT_S:
                    timed_out.append((counter, sent_str))
                    del state.in_flight[counter]
        if timed_out:
            with state.lock:
                state.lost += len(timed_out)
            sample = ", ".join(str(c) for c, _ in timed_out[:5])
            extra = f" (+{len(timed_out) - 5} more)" if len(timed_out) > 5 else ""
            term.err(f"LOST {COUNTER_FIELD}={sample}{extra}")
            for counter, sent_str in timed_out:
                csv_log("LOST", counter, "",
                        datetime.now().isoformat(timespec="seconds"),
                        None, sent_str, "", "timeout")


# =====================================================================
# Periodic summary
# =====================================================================

def summary_thread():
    while not shutdown_evt.is_set():
        shutdown_evt.wait(timeout=SUMMARY_INTERVAL_S)
        if shutdown_evt.is_set():
            break
        print_summary()


def print_summary():
    with state.lock:
        sent    = state.sent
        ok      = state.echoed_ok
        bad     = state.echoed_bad
        lost    = state.lost
        unexp   = state.unexpected
        oord    = state.out_of_order
        in_fl   = len(state.in_flight)
        avg_rtt = (state.rtt_sum_ms / state.rtt_count) if state.rtt_count else 0.0
        mn      = state.rtt_min_ms if state.rtt_count else 0.0
        mx      = state.rtt_max_ms
    pct_ok = (ok / sent * 100.0) if sent else 0.0
    line = (
        f"SUMMARY  sent={sent}  match={ok} ({pct_ok:.1f}%)  "
        f"mismatch={bad}  lost={lost}  unexpected={unexp}  "
        f"out-of-order={oord}  in-flight={in_fl}  "
        f"rtt_ms min/avg/max={mn:.1f}/{avg_rtt:.1f}/{mx:.1f}"
    )
    term.summary(line)


# =====================================================================
# Signal handlers
# =====================================================================

def install_signal_handlers():
    def _sig(signum, frame):
        term.info(f"Signal {signum} received; finishing up…")
        shutdown_evt.set()
    signal.signal(signal.SIGINT,  _sig)
    signal.signal(signal.SIGTERM, _sig)


# =====================================================================
# Main
# =====================================================================

def parse_args():
    p = argparse.ArgumentParser(description="MQTT round-trip echo test")
    p.add_argument("--broker", "-b", default=DEFAULT_BROKER,
                   help=f"Broker IP/host (default {DEFAULT_BROKER})")
    p.add_argument("--port", "-p", type=int, default=DEFAULT_PORT)
    p.add_argument("--tx-topic", default=DEFAULT_TX_TOPIC,
                   help=f"Topic to publish requests on (default {DEFAULT_TX_TOPIC})")
    p.add_argument("--rx-topic", default=DEFAULT_RX_TOPIC,
                   help=f"Topic to listen for echoes (default {DEFAULT_RX_TOPIC})")
    p.add_argument("--interval-ms", type=int, default=DEFAULT_INTERVAL_MS,
                   help=f"Time between sends in ms (default {DEFAULT_INTERVAL_MS})")
    p.add_argument("--count", type=int, default=0,
                   help="Stop after N messages (0 = no limit)")
    p.add_argument("--duration", type=int, default=0,
                   help="Stop after S seconds (0 = no limit)")
    return p.parse_args()


def main():
    args = parse_args()
    install_signal_handlers()
    path = open_csv()

    term.banner("MQTT round-trip echo test")
    term.info(f"Broker:    {args.broker}:{args.port}")
    term.info(f"Send on:   {args.tx_topic}")
    term.info(f"Listen on: {args.rx_topic}")
    term.info(f"Rate:      every {args.interval_ms} ms")
    term.info(f"Counter:   '{COUNTER_FIELD}' field, starts at 1, increments per message")
    if args.count:    term.info(f"Stop after {args.count} messages")
    if args.duration: term.info(f"Stop after {args.duration} seconds")
    term.info(f"CSV log:   {path}")
    term.info("Press Ctrl+C to stop early.\n")

    client = mqtt.Client(
        client_id=f"echo_test_{os.getpid()}",
        protocol=mqtt.MQTTv311,
        userdata={"rx_topic": args.rx_topic},
    )
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=10)

    try:
        client.connect_async(args.broker, args.port, keepalive=30)
    except Exception as e:
        term.err(f"connect_async failed: {e}")
    client.loop_start()

    threads = [
        threading.Thread(target=publisher_thread,  args=(client, args), daemon=True),
        threading.Thread(target=timeout_sweeper,                       daemon=True),
        threading.Thread(target=summary_thread,                        daemon=True),
    ]
    for t in threads:
        t.start()

    try:
        while not shutdown_evt.is_set():
            shutdown_evt.wait(timeout=0.5)
    except KeyboardInterrupt:
        shutdown_evt.set()

    print()
    term.banner("Final summary")
    print_summary()

    client.loop_stop()
    try: client.disconnect()
    except Exception: pass
    close_csv()
    term.info(f"CSV saved to {path}")


if __name__ == "__main__":
    main()