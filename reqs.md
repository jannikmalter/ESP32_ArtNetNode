# ESP32 ArtNetNode: Requirements, Goals, Bugs, TODOs

Single source of truth for the firmware ([src/main.c](src/main.c)). Consolidates
the former `requirements.md` and `bugs.md`. Covers goals, functional requirements
with status, the bug audit, and the open backlog.

Status values:

- **Completed**: implemented in `src/main.c` (and verified where noted)
- **Partial**: implemented for some targets/cases only
- **Open**: not implemented / not fixed
- **By design**: reviewed, intentional, no change planned

---

## 1. Goals

- Turn an ESP32 Ethernet board into a reliable Art-Net to DMX512 node for live lighting.
- Reliability and performance first: stable output, bounded latency, deterministic DMX timing.
- Bounds-checked handling of all untrusted network input.
- No external libraries; interface ESP-IDF directly.
- One codebase for both target boards (Olimex ESP32-POE deployed, WT32-ETH01 secondary) and both device variants (rack, mini).

---

## 2. Requirements

### 2.1 System & runtime

| ID | Requirement | Status | Notes |
|---|---|---|---|
| R1 | ESP32 acts as an Art-Net to DMX node | Completed | |
| R5 | Separate tasks for UDP ingest, TCP config, DMX generation | Completed | `eth_task` / `tcp_task` / `dmx_task` |
| R6 | Persist settings to NVS | Completed | namespace `"storage"` |
| R7 | No external libraries; ESP-IDF APIs only | Completed | |

### 2.2 DMX generation

| ID | Requirement | Status | Notes |
|---|---|---|---|
| R3 | Generate DMX512 on 7 output pins | Completed | `dmx_task`, `GPIO_patch[7]` |
| R8 | DMX512 produced in software by bit-banging | Completed | |
| R9 | One core reserved exclusively for DMX | Completed | core 1, pinned, max priority |
| R10 | Watchdog and scheduler held off on the DMX core | Completed | `vPortEnterCritical` + INT/TASK WDT on CPU1 disabled |
| R11 | 250 kbaud, timing by CPU cycle counting | Completed | 960 cycles/bit at 240 MHz |
| R12 | Adjustable channel count to raise frame rate | Completed | `NUM_CHAN` 1..512 |
| R13 | DMX sync to a selected input universe, or free-run | Completed | `synchronize` / `sync_addr` / `trigger`; sync reliability reworked in R24 |
| R24 | Rework DMX sync into a "new data" flag with clear-before-send semantics: set the flag when a sync-universe ArtDMX packet is processed or an ArtSync packet (0x5200) arrives; `dmx_task` waits for the flag and clears it before sending each frame, so a packet arriving mid-frame is not lost | Open | reworks R13; current `trigger` stalls in BREAK ([src/main.c:244-246](src/main.c#L244-L246)) and clears after the frame ([src/main.c:299](src/main.c#L299)), dropping late packets; adds ArtSync (OpSync 0x5200) handling in `eth_task`; keep the ~0.2 s failsafe timeout; see T9 |

### 2.3 Art-Net & network

| ID | Requirement | Status | Notes |
|---|---|---|---|
| R2 | Receive Art-Net DMX over UDP on wired Ethernet (port 6454) | Completed | `eth_task` |
| R14 | Obtain IPv4 address via DHCP | Completed | `ESP_NETIF_DEFAULT_ETH` DHCP client |
| R15 | Fall back to a link-local address when no DHCP is available | Open | not implemented; see T1 |
| R16 | 7-universe virtual patch, any universe routable to any output | Completed | `DMX_patch` / `DMX_repatch`, fan-out supported |
| R18 | Respond to ArtPoll with ArtPollReply | Completed | opcodes 0x2000/0x6000/0x7000 -> 0x2100 |
| R21 | Bounds-checked, signed-safe Art-Net ingest path | Completed | see B1, B2 |
| R22 | Native Art-Net output configuration and query: set the per-output universe patch and node name from a console via ArtAddress (0x6000), and report current config in ArtPollReply | Open | replaces the current 0x6000/0x7000 ArtPoll-trigger quirk ([src/main.c:724](src/main.c#L724)); touches B12; field layouts in [art-net.pdf](art-net.pdf); see T7 |

### 2.4 Configuration interfaces

| ID | Requirement | Status | Notes |
|---|---|---|---|
| R4 | Runtime configuration over a raw TCP interface (port 1337) | Completed | `tcp_task` |
| R17 | Patch and settings configurable over the raw TCP interface | Completed | `PATCH` / `SYNC` / `SYNC_ADDR` / `NUM_CHAN` |
| R23 | Replace the TCP CLI with a minimal web UI exposing the same settings and live state | Open | ESP-IDF `esp_http_server` only (R7); reuse stop/start + NVS handshake; replaces R4/R17; see T8 |

### 2.5 Hardware targets & build

| ID | Requirement | Status | Notes |
|---|---|---|---|
| R19 | Compile-time board switch: Olimex ESP32-POE and WT32-ETH01 | Partial | Olimex completed and deployed; WT32 ETH config done, DMX pin maps are placeholders (T5) |
| R20 | Compile-time device variants: rack and mini | Partial | Olimex completed; WT32 variant pin maps TBD |

### 2.6 Non-functional

- Reliability and performance are the top priority (live lighting).
- Output stability and bounded latency across reconfiguration.
- Bounds-checked buffer math on all untrusted network input.

---

## 3. Bugs

Audit of `src/main.c` against the committed baseline. Severity and "masked"
(whether the bug is latent in the deployed Olimex setup) are noted per entry.

### 3.1 Open

#### B7. recvfrom and socket errors unhandled (Medium)
- Where: `eth_task`. `recv_len` declared `uint32_t` ([src/main.c:667](src/main.c#L667)), socket create ([src/main.c:677](src/main.c#L677)), recvfrom and length check ([src/main.c:690-691](src/main.c#L690-L691)).
- `recv_len` is unsigned, so a `-1` from `recvfrom` becomes `0xFFFFFFFF`, passes the `>= 10` check, and a stale `ArtNetBuf` is processed. The socket fd `s` is also an unchecked `uint32_t`; if `socket()` fails the loop becomes a tight CPU spin.
- Masked: sockets normally succeed in deployment.
- Fix direction: use a signed length, check `socket()` and `recvfrom()` returns, continue on error.

#### B11. num_chan not clamped on NVS load (Low)
- Where: clamp exists only in the `NUM_CHAN` setter ([src/main.c:631-633](src/main.c#L631-L633)); `load_sync_state` does not clamp ([src/main.c:397-405](src/main.c#L397-L405)).
- A stored value outside 1..512 (downgrade, corruption) is used directly, making the frame-length math `(num_chan+1)*11+...` invalid. Tied to B4.
- Fix direction: clamp `num_chan` to 1..512 after load.

#### B12. Hardcoded "Rack" node name regardless of variant (Low, cosmetic)
- Where: TCP banner ([src/main.c:546](src/main.c#L546)), ArtPollReply short/long name ([src/main.c:738-739](src/main.c#L738-L739)).
- A `mini` build still announces "Rack" on the network and in the banner. Documented as a known wart in [CLAUDE.md](CLAUDE.md).
- Fix direction: derive the name from `VARIANT_NAME`. See T4.

### 3.2 Fixed

| ID | Severity | Area | Issue | Resolution |
|---|---|---|---|---|
| B1 | High | Art-Net parse | Signed-`char` byte assembly sign-extends length/universe: huge `DMXlength` (heap overflow) or silently dropped universes | Cast each byte to `uint8_t` before the shift/OR |
| B2 | High | Art-Net parse | No `DMXlength` bound before `memcpy` | Guard `recv_len >= 18`; clamp `DMXlength` to 512 then to `recv_len - 18` |
| B3 | High | TCP parser | NULL `strtok` deref on separator-only or argument-less commands (remote DoS) | Null-check first token; wrap each setter argument in `if (token != NULL)` |
| B4 | High | NVS / boot | Uninitialized locals used when NVS keys absent | Initialize locals to 0 / 0 / 512 before `nvs_get_*` |
| B6 | Medium | Art-Net | ArtPollReply advertised 0.0.0.0 | Store device IP from `IP_EVENT_ETH_GOT_IP`; copy into reply bytes 10-13 |
| B8 | Low | Robustness | Unchecked `calloc` / `nvs_open` / task-create / `esp_netif_new` returns | Log + `esp_restart()` on boot-critical failures; save paths call `startDMX()` on `nvs_open` failure |
| B9 | Low | DMX init | Signed `1 <<` shift in bitmask build (UB at pin 31) | Use `1u <<` |
| B10 | Low | DMX core | Read-modify-write `|=` on W1TS/W1TC registers | Plain `=` write |
| B13 | Low | Timing | `vTaskDelay` tick-vs-ms mismatch in `eth_task` reconfig poll | Use `vTaskDelay(1)` |
| B14 | Low | Concurrency | `stopDMX` used a fixed-delay timing assumption; shared flags not `volatile` | Added `dmxStopped` ack handshake; `stopFlag` / `dmxStopped` / `trigger` marked `volatile` |

### 3.3 By design

#### B5. update_dmx_ptr() runs after startDMX()
- Where: `save_dmx_patch` calls `startDMX()` before `update_dmx_ptr()` ([src/main.c:594](src/main.c#L594)), so the repatch runs while `dmx_task` is live.
- Reviewed and confirmed memory-safe: `DMX_repatch[i]` is always in `[0,6]` and written atomically, so `dmx_task`'s `DMXbuf[512*DMX_repatch[i] + curchan-1]` stays in bounds whichever value wins the race; the `memset` only yields old-or-zero bytes. The stop/start window exists for the NVS write (flash-cache vs. the core-1 critical section), not the repatch. Only effect is a transient frame on re-patch.
- Optional polish tracked as T6.

---

## 4. TODOs / Backlog

| ID | Task | Related | Priority |
|---|---|---|---|
| T1 | Implement link-local (AUTOIP) fallback when DHCP is unavailable (enable `CONFIG_LWIP_AUTOIP`, start on DHCP timeout) | R15 | Medium |
| T2 | Handle socket and `recvfrom` errors in `eth_task` (signed length, check returns) | B7 | Medium |
| T3 | Clamp `num_chan` to 1..512 on NVS load | B11 | Low |
| T4 | Derive node name from `VARIANT_NAME` (banner + ArtPollReply) | B12 | Low |
| T5 | Define and verify WT32-ETH01 DMX pin maps on hardware (currently placeholders) | R19, R20 | Medium (when WT32 built) |
| T6 | Optional: move `update_dmx_ptr()` inside the stop/start window for glitch-free re-patch | B5 | Optional |
| T7 | Implement native Art-Net config/query: ArtAddress (set + persist universe patch and node name), full ArtPollReply (per-port universes, name, status), optional ArtInput (0x7000); read [art-net.pdf](art-net.pdf) | R22 | Medium |
| T8 | Build minimal web UI: one `esp_http_server` config page (patch, sync, sync universe, channels/output + live refresh), reuse the NVS handshake, decide TCP CLI removal | R23 | Medium |
| T9 | Rework sync: replace `trigger` with a "new data" flag set on sync-universe ArtDMX and on ArtSync (0x5200); in `dmx_task` wait for the flag and clear it at frame start (before send); keep the failsafe timeout | R24 | High |
