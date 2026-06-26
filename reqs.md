# ESP32 ArtNetNode — Requirements

Status: Active · Updated: 2026-06-26

Single source of truth for the firmware ([src/main.c](src/main.c)). Consolidates
the former `requirements.md` and `bugs.md`. Covers goals, functional requirements
with status, the bug audit, and the open backlog.

**Status legend.** `Done` is a single checkbox: ☑ = implemented in `src/main.c`
(as recorded; some items are as-built and verified only where noted), ☐ = not yet
done. Partial / open / by-design nuance is carried in the **Notes** column and in
the bug subsections, not in the checkbox.

## Goals
Why this exists. Everything below traces to one of these.

- **G1** — Turn an ESP32 Ethernet board into an Art-Net to DMX512 node for live lighting.
- **G2** — Reliability and performance first: stable output, bounded latency, deterministic DMX timing.
- **G3** — Bounds-checked handling of all untrusted network input.
- **G4** — No external libraries; interface ESP-IDF directly.
- **G5** — One codebase for both target boards (Olimex ESP32-POE deployed, WT32-ETH01 secondary) and both device variants (rack, mini).
- **G6** — Field-serviceable: configure and update the deployed node over the network, without USB reflashing or physical access.

## Out of scope
What this deliberately will *not* do (derived from project notes, not new policy).

- Wi-Fi / Bluetooth operation. The Olimex clock uses the internal APLL (`EMAC_CLK_OUT`); per ESP32 errata this precludes simultaneous Wi-Fi/BT. (from [info.md](info.md) gotchas)
- LED "pixelnode" functionality. The earlier half-finished pixelnode `src/main.c` was fully replaced by this Art-Net/DMX firmware. (from [info.md](info.md))

## Requirements
One row each. Use "shall". `Type`: F=function, Q=quality, C=constraint.
`Pri`: M/S/C (must/should/could). `Goal`: the goal served.

### System & runtime

| ID | Type | Requirement | Pri | Goal | Done | Notes |
|----|------|-------------|-----|------|------|-------|
| R1 | F | The node shall act as an Art-Net to DMX node | M | G1 | ☑ | |
| R5 | C | The firmware shall use separate tasks for UDP ingest, config, and DMX generation | M | G2 | ☑ | `eth_task` / `httpd` (web UI) / `dmx_task`; config task was `tcp_task` until the TCP CLI was removed (R4/R17) |
| R6 | F | The node shall persist settings to NVS | M | G1 | ☑ | namespace `"storage"` |
| R7 | C | The firmware shall use no external libraries; ESP-IDF APIs only | M | G4 | ☑ | |

### DMX generation

| ID | Type | Requirement | Pri | Goal | Done | Notes |
|----|------|-------------|-----|------|------|-------|
| R3 | F | The node shall generate DMX512 on 7 output pins | M | G1 | ☑ | `dmx_task`, `GPIO_patch[7]` |
| R8 | F | DMX512 shall be produced in software by bit-banging | M | G2 | ☑ | |
| R9 | C | One core shall be reserved exclusively for DMX | M | G2 | ☑ | core 1, pinned, max priority |
| R10 | C | Watchdog and scheduler shall be held off on the DMX core | M | G2 | ☑ | `vPortEnterCritical` + INT/TASK WDT on CPU1 disabled |
| R11 | Q | DMX shall run at 250 kbaud with timing by CPU cycle counting | M | G2 | ☑ | 960 cycles/bit at 240 MHz |
| R12 | F | The node shall support an adjustable channel count to raise frame rate | S | G2 | ☑ | `NUM_CHAN` 1..512 |
| R13 | F | The node shall sync DMX to a selected input universe, or free-run | S | G2 | ☑ | `synchronize` / `sync_addr` / `trigger`; sync reliability reworked in R24 |
| R24 | F | The node shall rework DMX sync into a "new data" flag with clear-before-send semantics, so a packet arriving mid-frame is not lost | M | G2 | ☐ | reworks R13; see [reqs/R24.md](reqs/R24.md), T9 |

### Art-Net & network

| ID | Type | Requirement | Pri | Goal | Done | Notes |
|----|------|-------------|-----|------|------|-------|
| R2 | F | The node shall receive Art-Net DMX over UDP on wired Ethernet (port 6454) | M | G1 | ☑ | `eth_task` |
| R14 | F | The node shall obtain an IPv4 address via DHCP | M | G1 | ☑ | `ESP_NETIF_DEFAULT_ETH` DHCP client |
| R15 | F | The node shall fall back to a link-local address when no DHCP is available | S | G1 | ☐ | not implemented; see T1 |
| R16 | F | The node shall provide a 7-universe virtual patch, any universe routable to any output | M | G1 | ☑ | `DMX_patch` / `DMX_repatch`, fan-out supported |
| R18 | F | The node shall respond to ArtPoll with ArtPollReply | S | G1 | ☑ | ArtPoll 0x2000 -> ArtPollReply 0x2100; since R22 the reply is fully populated (one bound port per output). The 0x6000/0x7000-as-poll quirk was removed (R22) — they now have their correct ArtAddress/ArtInput meaning |
| R21 | Q | The Art-Net ingest path shall be bounds-checked and signed-safe | M | G3 | ☑ | see B1, B2 |
| R22 | F | The node shall support native Art-Net output configuration and query (set per-output universe patch and node name via ArtAddress; report config in ArtPollReply) | S | G1 | ☑ | **Done 2026-06-26.** ArtPollReply reports all 7 outputs as bound ports (Art-Net 4 per-port binding, BindIndex 1..7, one universe per reply); ArtAddress (0x6000) sets per-output universe + node name, persisted via the stop/start+NVS handshake; the old 0x6000/0x7000 ArtPoll-trigger quirk removed (closes B12). ArtInput (0x7000) out of scope (output-only node). See [reqs/R22.md](reqs/R22.md), T7 |

### Configuration interfaces

| ID | Type | Requirement | Pri | Goal | Done | Notes |
|----|------|-------------|-----|------|------|-------|
| R4 | F | ~~The node shall support runtime configuration over a raw TCP interface (port 1337)~~ | M | G1 | ☑ | **Removed 2026-06-26** — superseded by R23 (web UI). `tcp_task` deleted; port 1337 no longer served |
| R17 | F | ~~Patch and settings shall be configurable over the raw TCP interface~~ | M | G1 | ☑ | **Removed 2026-06-26** — superseded by R23. Same settings now configured via the web UI `/api/config` |
| R23 | F | The node shall replace the TCP CLI with an interactive web UI exposing the same settings and live state | M | G6 | ☑ | Web UI on `esp_http_server` (port 80, core 0): `/`, `/api/state`, `/api/config`, `/api/ota`; reuses stop/start + NVS handshake. **TCP CLI removed 2026-06-26** (R4/R17 retired). See [reqs/R23.md](reqs/R23.md), T8 |
| R27 | Q | The web UI shall be lightweight, fast-loading, and visually polished | S | G6 | ☑ | single self-contained page ~8.5 KB (config + live graphs + OTA upload), served from flash, no CDNs/libs (R7), responsive; [src/index.html](src/index.html) |
| R28 | F | The web UI shall display rolling 1-minute history graphs of Art-Net packet rate and DMX refresh rate, built client-side from periodic state polls (no on-device logging) | S | G6 | ☑ | firmware counts Art-Net packets/sec in `eth_task`, exposes it as `pps` in `/api/state`; graphs drawn client-side in [src/index.html](src/index.html). See [reqs/R28.md](reqs/R28.md), T13 |
| R29 | F | The node shall measure and report core-0 CPU utilization, shown in the web UI as a live value and 1-minute history graph | S | G2 | ☑ | `stats_task` computes load from core-0 IDLE run-time over a 1 s window (FreeRTOS run-time stats); exposed as `load0` in `/api/state`, graphed client-side. Core 1 is unmeasurable by design (owns its core in a critical section). Needs `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`. See [reqs/R29.md](reqs/R29.md), T14 |

### Firmware update (OTA)

| ID | Type | Requirement | Pri | Goal | Done | Notes |
|----|------|-------------|-----|------|------|-------|
| R26 | F | The node shall support OTA firmware update by uploading an image through the web UI | M | G6 | ☑ | primary OTA channel; `POST /api/ota` streams the raw image through `esp_ota_begin/write/end` into the inactive slot then reboots, under the `stopDMX()`/`startDMX()` handshake (DMX fully stopped during update); dual slots + rollback-confirm foundation already in place; upload control + progress in [src/index.html](src/index.html). See [reqs/R26.md](reqs/R26.md), T10-T11 |
| R25 | F | The node shall, where the protocol proves tractable, also accept OTA over the ArduinoOTA/espota protocol so PlatformIO's `upload_protocol = espota` flashes it directly | C | G6 | ☐ | conditional on a protocol-feasibility spike; reuses PlatformIO's bundled `espota.py` as host, no bespoke host program; see [reqs/R25.md](reqs/R25.md), T12 |

### Hardware targets & build

| ID | Type | Requirement | Pri | Goal | Done | Notes |
|----|------|-------------|-----|------|------|-------|
| R19 | C | The firmware shall provide a compile-time board switch: Olimex ESP32-POE and WT32-ETH01 | M | G5 | ☐ | Partial: Olimex completed and deployed; WT32 ETH config done, DMX pin maps are placeholders (T5) |
| R20 | C | The firmware shall provide compile-time device variants: rack and mini | M | G5 | ☐ | Partial: Olimex completed; WT32 variant pin maps TBD |

### Non-functional

Quality attributes; trace to G2 (reliability/performance) and G3 (input safety).

- Reliability and performance are the top priority (live lighting). (G2)
- Output stability and bounded latency across reconfiguration. (G2)
- Bounds-checked buffer math on all untrusted network input. (G3, R21)

## Bugs
Deviations from a requirement. `Ref` = the requirement broken. `Sev`: Hi/Md/Lo.
Audit of `src/main.c` against the committed baseline; "masked" (whether latent in
the deployed Olimex setup) is noted in the detail files.

### Open

| ID | Bug | Ref | Sev | Done |
|----|-----|-----|-----|------|
| B11 | `num_chan` not clamped on NVS load | R12 | Lo | ☐ |

Detail: [reqs/B11.md](reqs/B11.md), [reqs/B12.md](reqs/B12.md).

### Fixed

| ID | Area | Bug | Ref | Sev | Done | Resolution |
|----|------|-----|-----|-----|------|------------|
| B1 | Art-Net parse | Signed-`char` byte assembly sign-extends length/universe: huge `DMXlength` (heap overflow) or silently dropped universes | R21 | Hi | ☑ | Cast each byte to `uint8_t` before the shift/OR |
| B2 | Art-Net parse | No `DMXlength` bound before `memcpy` | R21 | Hi | ☑ | Guard `recv_len >= 18`; clamp `DMXlength` to 512 then to `recv_len - 18` |
| B3 | TCP parser | NULL `strtok` deref on separator-only or argument-less commands (remote DoS) | R17 | Hi | ☑ | Null-check first token; wrap each setter argument in `if (token != NULL)` |
| B4 | NVS / boot | Uninitialized locals used when NVS keys absent | R6 | Hi | ☑ | Initialize locals to 0 / 0 / 512 before `nvs_get_*` |
| B6 | Art-Net | ArtPollReply advertised 0.0.0.0 | R18 | Md | ☑ | Store device IP from `IP_EVENT_ETH_GOT_IP`; copy into reply bytes 10-13 |
| B8 | Robustness | Unchecked `calloc` / `nvs_open` / task-create / `esp_netif_new` returns | R1 | Lo | ☑ | Log + `esp_restart()` on boot-critical failures; save paths call `startDMX()` on `nvs_open` failure |
| B9 | DMX init | Signed `1 <<` shift in bitmask build (UB at pin 31) | R3 | Lo | ☑ | Use `1u <<` |
| B10 | DMX core | Read-modify-write `\|=` on W1TS/W1TC registers | R8 | Lo | ☑ | Plain `=` write |
| B7 | Art-Net ingest | recvfrom/socket errors unhandled: unsigned length made a `-1` return look like `0xFFFFFFFF` and process a stale buffer; socket fd unchecked | R21 | Md | ☑ | Signed `recv_len` / `s`; check `socket()` (restart on failure); added `SO_RCVTIMEO` so an idle `recvfrom` returns `-1` and is skipped by the `>= 10` guard (landed with R28's packets/sec window) |
| B13 | Timing | `vTaskDelay` tick-vs-ms mismatch in `eth_task` reconfig poll | R13 | Lo | ☑ | Use `vTaskDelay(1)` |
| B14 | Concurrency | `stopDMX` used a fixed-delay timing assumption; shared flags not `volatile` | R5 | Lo | ☑ | Added `dmxStopped` ack handshake; `stopFlag` / `dmxStopped` / `trigger` marked `volatile` |
| B12 | Naming | Hardcoded "Rack" node name regardless of variant (cosmetic) | R20 | Lo | ☑ | Fixed with R22 (2026-06-26): `node_short_name`/`node_long_name` seeded from `VARIANT_NAME`, loaded from NVS, and settable over Art-Net (ArtAddress). T4 subsumed |

### By design

**B5 — `update_dmx_ptr()` runs after `startDMX()`** (Ref R16, reviewed — not a defect)
- Where: `save_dmx_patch` calls `startDMX()` before `update_dmx_ptr()` ([src/main.c:594](src/main.c#L594)), so the repatch runs while `dmx_task` is live.
- Reviewed and confirmed memory-safe: `DMX_repatch[i]` is always in `[0,6]` and written atomically, so `dmx_task`'s `DMXbuf[512*DMX_repatch[i] + curchan-1]` stays in bounds whichever value wins the race; the `memset` only yields old-or-zero bytes. The stop/start window exists for the NVS write (flash-cache vs. the core-1 critical section), not the repatch. Only effect is a transient frame on re-patch.
- Optional polish tracked as T6.

## Todos

The development plan lives in [todo.md](todo.md) (items T1–T12), each referencing
the requirement/bug ID it advances.

---
*Pri:* M/S/C (must/should/could). *Sev:* Hi/Md/Lo. IDs are permanent — never reuse.
*Detail files: `reqs/<ID>.md` (e.g. `reqs/R24.md`, `reqs/B7.md`).*
