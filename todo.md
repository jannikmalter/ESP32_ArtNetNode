# Todos

Development plan. Each item references the [reqs.md](reqs.md) ID it advances;
priority in parentheses.

- [ ] **T1** — Implement link-local (AUTOIP) fallback when DHCP is unavailable
  (enable `CONFIG_LWIP_AUTOIP`, start on DHCP timeout). (R15, Medium)
- [x] **T2** — Handle socket and `recvfrom` errors in `eth_task` (signed length,
  check returns). **Done (2026-06-26):** signed `recv_len`/`s`, `socket()` checked
  (restart on failure), `SO_RCVTIMEO` so idle `recvfrom` returns are skipped;
  landed with R28. (B7, Medium)
- [ ] **T3** — Clamp `num_chan` to 1..512 on NVS load. (B11, Low)
- [x] **T4** — Derive node name from `VARIANT_NAME`. **Done (2026-06-26):**
  subsumed by T7 — `node_short_name`/`node_long_name` seed from `VARIANT_NAME`,
  load from NVS, and feed the ArtPollReply. (B12, Low)
- [ ] **T5** — Define and verify WT32-ETH01 DMX pin maps on hardware (currently
  placeholders). (R19, R20, Medium when WT32 built)
- [ ] **T6** — Optional: move `update_dmx_ptr()` inside the stop/start window for
  glitch-free re-patch. (B5, Optional)
- [x] **T7** — Implement native Art-Net config/query. **Done (2026-06-26):**
  ArtPollReply reports all 7 outputs as bound ports (Art-Net 4 per-port binding,
  one universe per reply, BindIndex 1..7); ArtAddress (0x6000) sets + persists the
  per-output universe and node name via the stop/start + NVS handshake; the old
  0x6000/0x7000 ArtPoll-trigger quirk removed. ArtInput (0x7000) left unhandled
  (output-only node). `send_artpollreply`/`handle_artaddress` in
  [src/main.c](src/main.c). (R22, Medium)
- [x] **T8** — Web UI. **Done (2026-06-25):** `esp_http_server` on core 0 (port 80)
  serving one self-contained page ([src/index.html](src/index.html), embedded via
  [gen_web_assets.py](gen_web_assets.py)) with `/api/state` + `/api/config`,
  reusing the stop/start + NVS handshake; lightweight/responsive (R27 ☑). OTA upload
  control added (T11). **TCP CLI removed (2026-06-26):** `tcp_task` deleted, R4/R17
  retired — the web UI is the sole config interface. (R23, R27, Medium)
- [ ] **T9** — Rework sync: replace `trigger` with a "new data" flag set on
  sync-universe ArtDMX and on ArtSync (0x5200); in `dmx_task` wait for the flag and
  clear it at frame start (before send); keep the failsafe timeout. (R24, High)
- [x] **T10** — Build the shared OTA engine. **Done (2026-06-26):** foundation
  (2026-06-25) — 4 MB flash, dual-slot partition table ([partitions.csv](partitions.csv)
  via `board_build.partitions`), `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, and the
  `app_main` rollback-confirm — plus the `esp_ota_begin/write/end` write path in
  `ota_post_handler` (its first caller, T11), run with DMX stopped (off core 1).
  ESP-IDF only. (R26, High)
- [x] **T11** — Add browser OTA upload. **Done (2026-06-26):** `POST /api/ota`
  streams the raw image into the inactive slot under the `stopDMX()`/`startDMX()`
  handshake and reboots; firmware-upload control + XHR progress bar in
  [src/index.html](src/index.html). DMX is fully stopped during the update (by
  decision — done between shows). (R26, High)
- [ ] **T12** — Optional: spike the ArduinoOTA/espota wire protocol (PlatformIO's
  `espota.py`; ESPHome's `espota2.py` for reference); if tractable, implement an
  espota-compatible UDP/TCP responder on the T10 engine and set
  `upload_protocol = espota` per env, so the PlatformIO Upload button flashes over
  Ethernet. Drop if the handshake is too gnarly. Depends on T10. (R25, Could)
- [x] **T13** — Live history graphs in the web UI. **Done (2026-06-26):**
  `eth_task` counts Art-Net packets/sec and publishes `artnet_pps` (exposed as
  `pps` in `/api/state`); [src/index.html](src/index.html) keeps 60 s of `pps` +
  `refresh` client-side and draws two `<canvas>` sparklines, no libs. (R28, Should)
- [x] **T14** — Core-0 CPU load metric. **Done (2026-06-26):**
  `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` enabled; `stats_task` (core 0) derives
  load from core-0 IDLE run-time over a 1 s window, published as `load0` in
  `/api/state`; third sparkline in [src/index.html](src/index.html). (R29, Should)
- [x] **T15** — Web UI node-name editing + polish. **Done (2026-06-26):** pencil
  dialog edits short/long name (tab = short, header = long), `/api/config` gains
  `sname`/`lname` (URL-decoded, `save_node_name()` path); plus the UI refinements —
  connection-health LED, single-row outputs with per-field dirty highlighting and a
  Reset button, mobile layout (4-wide grid, stacked settings, numeric keypads,
  native steppers), smaller/narrower buttons, footer identity line. (R30, R27,
  Should)
- [x] **T16** — Harden web-config input handling. **Done (2026-06-26):** JSON-escape
  node names in `/api/state` (`json_escape`), size response buffers for worst-case
  escaping, reject over-long queries/values, cap/clamp every field — so no
  web-entered value can break the node. (R31, Must)
