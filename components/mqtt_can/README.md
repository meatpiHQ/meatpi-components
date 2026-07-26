# mqtt_can — CAN ⇄ MQTT bridge building blocks

Legacy WiCAN's raw-frames-over-MQTT, rebuilt as bridge parts (meatpi
2026-07-22: "a bridge with configurable tx and Rx topics … both tx and
Rx safety gate"): the **`mqtt0` bridge endpoint** + the **`canmqtt`
translator** (legacy JSON, batched). Compose with a normal bridge row:

```jsonc
// bridge_manager settings
{"name":"br_mqtt_can", "a":"can", "b":"mqtt0", "translator":"canmqtt"}
```

Design + rulings + test plan: `TASK_mqtt_can.md`.

## Wire contract (byte parity with legacy `main/mqtt.c`)

- publish (device→broker), default topic `~/can/rx`
  (`~` = mqtt_manager's topic prefix, `wican/<device_id>`):
  `{"bus":"0","type":"rx","ts":34610,"frame":[{"id":123,"dlc":8,
  "rtr":false,"extd":false,"data":[1,…,8]},…]}` — frames batched.
- subscribe (broker→device), default `~/can/tx`: same shape,
  `"type":"tx"`; `ts` tolerated, `bus` number or string, `extd`
  implied for id > 0x7FF.

## Settings (`mqtt_can`, v1)

| key | default | notes |
|---|---|---|
| `enabled` | false | registers mqtt0 + canmqtt (reboot-to-apply) |
| `pub_topic` / `sub_topic` | `~/can/rx` / `~/can/tx` | fully configurable; `~` expands to the mqtt prefix |
| `qos` | 0 | publish QoS |
| `allow_rx` | **false** | GATE: bus traffic may leave the device |
| `allow_tx` | **false** | GATE: broker frames may reach the bus |
| `batch_ms` | 50 | max frame age before a batch flushes (pump flush hook) |
| `batch_frames` | 24 | flush at this many frames (cap `MC_BATCH_MAX`) |
| `filter`/`mask`/`filter_ext` | "" | hex; maps 1:1 onto the `can` jack's RX filter (`bep_can_set_filter`); empty mask = all |
| `cli` | true | `mqtt_can` stats command |

**Both gates default OFF** (project gate convention): an enabled
bridge moves NOTHING until each direction is explicitly consented.
Gate-blocked traffic is counted (`mqtt_can` CLI: rx-gate / tx-gate),
never silent.

## Mechanics

- RX: the `can` jack's chunk coalescing + the translator's in-ctx
  accumulator → one legacy-JSON batch per `mqtt_manager_publish_async`
  (never blocks the pump; broker-down drops are counted by
  mqtt_manager). Batches flush on `batch_frames`, or `batch_ms` via
  the bridge pump's translator-flush hook (added for this codec;
  `bridge_manager.h`).
- TX: one MQTT message → bridge chunks → the translator's stream
  reassembler (fragment-safe, brace/string-aware, oversize counted)
  → parse → frames onto the bus.
- The whole codec state lives in the translator ctx (`BM_CTX_MAX`
  4096) — nothing big on the 4 KB pump stack (stack-audit rule).
- `can` jack is **multi_consumer since 2026-07-26** (the v2 backlog
  item): slcan-on-can and mqtt-on-can run SIMULTANEOUSLY — the jack's
  pump fans each RX chunk out to every subscribed bridge (up to 4).
  Caveat: the jack has ONE ingress filter (this component's `filter`
  setting via `bep_can_set_filter`) — with several consumers attached
  it restricts what ALL of them see. Verified live: CANFAN BENCH.

## Memory

PSRAM: none beyond the shared bridge ctx slots. Internal: statics
< 1 KB (topics + stats). No task of its own — rides the bridge pump
and the esp-mqtt event task (handler only queues chunks).

## Tests

Host `host_test` 11/11 (legacy-exact strings incl. both documented
legacy examples, worst-case batch sizing vs `MC_JSON_MAX`, fragmented/
noisy/oversized tx streams, parse rejects). Live:
`tools/testbench/mqtt_can_bench_test.py` (→ `MQTT CAN BENCH PASS`) —
gates-closed leg FIRST, conservation-checked RX at rate, byte-exact TX
round-trip via PCAN, custom-topics leg.
