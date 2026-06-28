# Todos

Development plan. Each item references the [reqs.md](reqs.md) ID it advances;
priority in parentheses.

## Open

- [ ] **T5** — Define and verify WT32-ETH01 DMX pin maps on hardware (currently
  placeholders). (R19, R20, Medium when WT32 built)
- [ ] **T12** — Optional: spike the ArduinoOTA/espota wire protocol (PlatformIO's
  `espota.py`; ESPHome's `espota2.py` for reference); if tractable, implement an
  espota-compatible UDP/TCP responder on the T10 engine and set
  `upload_protocol = espota` per env, so the PlatformIO Upload button flashes over
  Ethernet. Drop if the handshake is too gnarly. Depends on T10. (R25, Could)

## Done

- [x] **T6** — Move `update_dmx_ptr()` inside the stop/start window for glitch-free
  re-patch. **Done (2026-06-28):** `save_dmx_patch` now calls `update_dmx_ptr()`
  between the NVS write and `startDMX()` (DMX parked during the buffer clear +
  repatch-table rebuild), and runs it even on NVS-open failure so the in-memory
  patch still applies; the two external `update_dmx_ptr()` calls (Art-Net + web
  `/api/config`) were removed. No more transient frame on re-patch. (B5, Optional)
- [x] **T3** — Clamp `num_chan` to 1..512 on NVS load. **Done (2026-06-28):**
  `load_sync_state` clamps `num_chan_load` to 1..512 before assignment, same bounds
  as the `/api/config` setter, so a stored out-of-range value can't break the
  frame-length math. (B11, Low)
- [x] **T1** — Link-local (AUTOIP) fallback when DHCP is unavailable. **Done
  (2026-06-28):** `CONFIG_LWIP_AUTOIP=y` in [sdkconfig.defaults](sdkconfig.defaults)
  enables lwIP's cooperative DHCP+AUTOIP — DHCP keeps running, the node self-assigns
  169.254.x.x after `CONFIG_LWIP_AUTOIP_TRIES` (2) failed DISCOVERs, and a later DHCP
  server takes over automatically. `got_ip_event_handler` now also handles
  `IP_EVENT_ETH_LOST_IP` so the runtime address change updates `device_ip`. Built clean
  (all envs re-seeded); not yet hardware-confirmed. (R15, Medium)
- [x] **T2** — Handle socket and `recvfrom` errors in `eth_task` (signed length,
  check returns). **Done (2026-06-26):** signed `recv_len`/`s`, `socket()` checked
  (restart on failure), `SO_RCVTIMEO` so idle `recvfrom` returns are skipped;
  landed with R28. (B7, Medium)
- [x] **T4** — Derive node name from `VARIANT_NAME`. **Done (2026-06-26):**
  subsumed by T7 — `node_short_name`/`node_long_name` seed from `VARIANT_NAME`,
  load from NVS, and feed the ArtPollReply. (B12, Low)
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
- [x] **T9** — Rework sync: the `trigger` flag (name kept) is now set on
  sync-universe ArtDMX and on ArtSync (`0x5200`); `dmx_task` waits for it and clears
  it at frame start (before send), so a mid-frame packet is reflected next frame, not
  lost; the ~0.2 s failsafe timeout is preserved. Clear-before-send verified on
  hardware. (R24, High)
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
  native steppers), smaller/narrower buttons, footer identity line, and an
  OS-following light/dark theme (`prefers-color-scheme`). (R30, R27, Should)
- [x] **T16** — Harden web-config input handling. **Done (2026-06-26):** JSON-escape
  node names in `/api/state` (`json_escape`), size response buffers for worst-case
  escaping, reject over-long queries/values, cap/clamp every field — so no
  web-entered value can break the node. (R31, Must)
- [x] **T17** — Double-buffer DMX for frame-perfect sync output. **Done (2026-06-28):**
  added `ArtSyncBuf` back buffer; in sync mode `eth_task` writes ArtDMX into it (free-run
  still writes `DMXbuf` directly), and `dmx_task` copies the whole back buffer into
  `DMXbuf` at the frame boundary when it consumes `trigger` — the only place the copy
  can't tear the frame. Free-run path unchanged. Built clean; snapshot not yet
  hardware-confirmed. (R32, Should)
- [x] **T18** — Make ArtSync a distinct third sync mode. **Done (2026-06-28):**
  `synchronize` is now `SYNC_OFF`/`SYNC_UNI`/`SYNC_ART`; both sync modes write the
  back buffer and snapshot at the frame edge, but ArtSync commits only on `0x5200`
  and universe-sync only on the `sync_addr` ArtDMX (no overlap). NVS 0/1 unchanged,
  value clamped; `/api/config` `sync=0|1|2`; web UI Mode = Free-run/Uni-Sync/ArtSync.
  Built clean; not yet hardware-confirmed. (R33, Should)
- [x] **T19** — Consolidate node naming into one suffix + add hostname. **Done
  (2026-06-28):** the user edits one `node_suffix`; `apply_node_names()` derives the
  Art-Net short (`"LF "`+suffix) and long (`"LICHTFETISCH ArtNet Node "`+suffix) names
  and the network hostname (`"LF-ArtNetNode-"`+sanitized suffix, set via
  `esp_netif_set_hostname` before DHCP). NVS key `SUFFIX` replaces `SHORTNAME`/`LONGNAME`;
  `/api/config` takes `suffix`; `/api/state` adds `suffix`/`host`; the web dialog edits
  the suffix with a live preview. ArtAddress name-writes removed (patch-only). All four
  envs built clean; not yet hardware-confirmed. (R30, R34, Should)
