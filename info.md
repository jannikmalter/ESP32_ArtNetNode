# info.md — how the firmware works

Internal documentation for the **ESP32 ArtNetNode** firmware: architecture,
design, and internal how-to for whoever (human or agent) works in the code. The
outward overview is [README.md](README.md); decided goals/requirements/bugs live
in [reqs.md](reqs.md); the development plan is [todo.md](todo.md). Bulky reference
material is in [docs/](docs/).

## What this project is

Firmware that turns an **ESP32 Ethernet board** into an **Art-Net → DMX512 node**.
**Two boards are supported via a compile-time switch: Olimex ESP32-POE (the only
hardware actually built so far) and WT32-ETH01.**

- Receives Art-Net DMX data over **UDP via wired Ethernet**.
- Generates **DMX512 output on seven GPIO pins** via software bit-banging.
- Configurable at runtime over a **web UI** (the former raw TCP CLI was removed)
  and natively over **Art-Net** (ArtAddress sets the per-output universe patch and
  node name; ArtPollReply reports them — R22).
- Responds to **ArtPoll** discovery so it appears in lighting controllers,
  advertising each output as its own bound port with an independent universe.
- Persists configuration to **NVS** (non-volatile storage).

**Reliability and performance are the top priorities** (it drives live lighting).
No external libraries — the firmware talks to ESP-IDF directly.

Protocol reference: [docs/art-net.pdf](docs/art-net.pdf). DMX512 standard:
[docs/ANSI E1.11 - 2024.pdf](docs/ANSI%20E1.11%20-%202024.pdf).

## How the firmware is structured

**`src/main.c` is the firmware.** Its DMX engine, Art-Net/UDP handling, TCP config
interface and NVS logic were **ported verbatim from the original working OLIMEX
ESP32-POE firmware** (ESP-IDF ~v4.0); only the **Ethernet bring-up was modernized**
to the ESP-IDF v5 API and **parameterized to build for both boards** (Olimex
ESP32-POE + WT32-ETH01) and both device variants (rack/mini). (The earlier
half-finished LED "pixelnode" `src/main.c` has been fully replaced.)

**Source-of-truth rule:** the DMX/Art-Net/TCP logic is the proven OLIMEX reference
logic — preserve its behavior exactly when changing it (the port was verified
line-for-line identical apart from the documented deltas below). For Ethernet/init
questions, the IDF 6.0 patterns described below are the modern guide. Deployed
hardware is the **Olimex ESP32-POE**; WT32-ETH01 is the secondary target.

> **Note:** the original `ArtNetNode_OLIMEX_POE_v001/` reference firmware and the
> vendored `esp32 ethernet examples/` (Espressif's IDF 6.0.1 `basic` Ethernet
> example) have been **removed from the repo** after the port was verified
> line-for-line. They were untracked, so they are **not** recoverable from git
> history — the technical facts distilled from them are retained in this document
> instead. The Espressif Ethernet example is reproducible from
> `esp-idf/examples/ethernet/basic`; the OLIMEX original, if ever needed again,
> must come from a separate backup or upstream source.

### What changed vs. the original (all in `src/main.c`)
- **Ethernet init** rewritten for IDF v5 (`eth_esp32_emac_config_t`, 2-arg
  `esp_eth_mac_new_esp32`, `esp_eth_phy_new_lan87xx`, in-code RMII clock config),
  selected per board by `#ifdef`.
- **Board + variant compile-time switch** (`BOARD_*` / `VARIANT_*`, set in
  [platformio.ini](platformio.ini)) drives both the ETH config block and the
  `GPIO_patch[]` pin map.
- **`ETH_EVENT` link-state handler** added (logs link up/down) alongside
  `IP_EVENT_ETH_GOT_IP`; startup log prints board + variant.
- **v4→v5 fix:** `GPIO_PIN_INTR_DISABLE` → `GPIO_INTR_DISABLE` in `dmx_task`.

Everything else — `dmx_task`, `eth_task`, NVS save/load, `update_dmx_ptr` — is the
original logic unchanged. (The original raw TCP config CLI `tcp_task` was later
removed in favour of the web UI — R23/R4/R17.)

## The DMX512 bit-bang engine (`dmx_task` in `src/main.c`)

This is the heart of the project. See [src/main.c:170](src/main.c#L170). How it works:

- **Runs alone on core 1** at max priority, inside `vPortEnterCritical()` —
  interrupts and the scheduler are held off so timing is deterministic. It owns
  the core completely.
- **All 7 outputs are driven in parallel, bit-by-bit.** For each DMX bit-time it
  computes `outbuf`, a bitmask with one bit per output, then writes every output
  at once via the GPIO set/clear registers:
  - `GPIO_w1ts` = `0x3ff44008` (`GPIO_OUT_W1TS_REG`, set bits), `GPIO_w1tc` =
    `0x3ff4400c` (`GPIO_OUT_W1TC_REG`, clear bits). These only cover GPIO0–31, so
    **all DMX pins must be ≤ 31**.
- **Timing is cycle-counted**, not timer-driven: `xthal_get_ccount()`, busy-waiting
  **960 CPU cycles per bit**. 960 cycles ÷ 240 MHz = **4 µs = 250 kbaud**. ⇒ **the
  CPU must run at 240 MHz** or the baud rate is wrong.
- **DMX frame layout** (per bit counter `curbit`): `BREAK` = 30 bits low, then
  `MAB` = 10 bits high, then per channel 11 bit-times = 1 start bit (low) + 8 data
  bits (**LSB first**) + 2 stop bits (high). Channel data:
  `DMXbuf[512 * DMX_repatch[i] + curchan - 1]`.
- **Frame length is variable**: a frame ends at `(num_chan + 1) * 11 + BREAK + MAB`.
  Lowering `num_chan` (channels per output, 1–512) shortens the frame ⇒ **higher
  refresh rate**. `refresh_rate` (Hz) is measured from the cycle delta between
  frames.
- **Sync vs. free-run**: if `synchronize`, the generator stalls the break until
  `trigger` is set (the `eth_task` sets `trigger` when an ArtDMX frame for
  `sync_addr` arrives), with a ~50M-cycle (~0.2 s) failsafe timeout so it never
  hangs. If not synced, it free-runs.
- **Safe reconfiguration**: `stopDMX()` sets `stopFlag` (generator exits the
  critical section and idles) → write NVS / change buffers → `startDMX()` clears
  it. `eth_task` also pauses on `stopFlag`.
- **Parked at boot**: `stopFlag` starts **set**, so DMX output and Art-Net input
  stay parked through all of `app_main`; `startDMX()` is called **last**, only once
  boot has fully succeeded. This keeps every boot-time flash op safe (the bit-bang
  generator never holds core 1's critical section while the cache is stopped — see
  [Firmware update (OTA)](#firmware-update-ota)) and means a boot that faults out
  never drives the lines with half-initialised state.

### Output multiplexing model

- `GPIO_patch[NUM_OUT]` — the physical GPIO for each of the 7 outputs, in device
  order. **Selected at compile time per board + variant** (see
  [Supported boards](#supported-boards-compile-time-switch)): Olimex rack
  `{16,15,14,13,5,4,2}`, Olimex mini `{5,2,4,0,16,14,15}`.
- `DMX_patch[NUM_OUT]` — the Art-Net universe (port-address) feeding each output.
  Set via the `PATCH` command, saved to NVS.
- `DMX_repatch[NUM_OUT]` — built by `update_dmx_ptr()`: if several outputs are
  patched to the *same* universe, they share one 512-byte buffer slot (dedup), so
  the same universe can fan out to multiple physical outputs.
- `DMXbuf` = `calloc(NUM_OUT * 512)` — one 512-channel block per output slot.
  `eth_task` does `memcpy(DMXbuf + bufaddress*512, ArtNetBuf+18, DMXlength)` on
  each matching ArtDMX packet.

## Supported boards (compile-time switch)

Both boards are ESP32 + a LAN87xx PHY over RMII, so they share the
`esp_eth_phy_new_lan87xx()` driver and the fixed RMII data pins. They differ in
PHY address, PHY-power pin, the RMII **clock direction**, and therefore which
GPIOs are free for DMX output. A board macro **and** a variant macro (per
PlatformIO env — `-DBOARD_OLIMEX_ESP32_POE`/`-DBOARD_WT32_ETH01` and
`-DVARIANT_RACK`/`-DVARIANT_MINI`) select the ETH config block and the
`GPIO_patch[]` DMX pin map. The four envs are `olimex-rack`, `olimex-mini`,
`wt32-rack`, `wt32-mini`.

| Parameter | **Olimex ESP32-POE** (deployed) | **WT32-ETH01** (secondary) |
|---|---|---|
| PlatformIO `board` | `esp32-poe` | `wt32-eth01` |
| PHY chip / driver | LAN8710A / `lan87xx` | LAN8720 / `lan87xx` |
| PHY address | **0** | **1** |
| PHY power GPIO | **12** | **16** |
| PHY reset GPIO | -1 (not wired) | -1 (not wired) |
| MDC / MDIO | 23 / 18 | 23 / 18 |
| RMII REF_CLK | **OUT on GPIO17** (`EMAC_CLK_OUT`, internal APLL) | **IN on GPIO0** (`EMAC_CLK_EXT_IN`, external 50 MHz osc) |
| RMII data (fixed by SoC) | TXD0 19, TXD1 22, TX_EN 21, RXD0 25, RXD1 26, CRS_DV 27 | same |
| **Free for DMX output** | 16 free (power is on 12); **17 taken by clock** | 17 free; **16 taken by power**; 0 taken by clock |
| **7 DMX output pins** | rack `{16,15,14,13,5,4,2}` · mini `{5,2,4,0,16,14,15}` — proven on hardware | rack `{17,15,14,13,5,4,2}` · mini `{5,2,4,12,17,14,15}` — **placeholders** (16→17, 0→12; verify against the WT32 header pinout) |

The original OLIMEX firmware set the clock via Kconfig
(`CONFIG_ETH_RMII_CLK_OUTPUT=y`, `CONFIG_ETH_RMII_CLK_OUT_GPIO=17`); `src/main.c`
now sets it **in code** per board via `esp32_emac_cfg.clock_config.rmii.*`.

**Per-board gotchas:**
- **DMX pins must be ≤ 31** for the W1TS/W1TC registers (`0x3ff44008`/`0c`) — so on
  WT32 avoid 32/33 even though they're otherwise free.
- **Olimex clock is the internal APLL** (`EMAC_CLK_OUT`). Per ESP32 errata, that
  rules out simultaneous Wi-Fi/BT and consumes the APLL — fine here (Ethernet-only
  node), but don't add Wi-Fi to the Olimex build.
- The Olimex DMX pins include strapping pins (0, 2, 5, 15) — the reference drives
  them as outputs *after* boot, which works; keep them low/floating at reset.

## ESP-IDF Ethernet bring-up: v4 → v5.5 → v6.0

The same EMAC+LAN8720-over-RMII init evolved across three IDF eras. This table
records how the original OLIMEX firmware (IDF ~v4.0) and Espressif's `basic`
example (IDF 6.0.1) did it versus the current `src/main.c` — kept as a
translation/reference guide even though those two sources are no longer vendored
in-tree.

| Concern | OLIMEX original (IDF ~v4.0) | Current `src/main.c` (IDF 5.5) | Official example (IDF 6.0.1) |
|---|---|---|---|
| ESP32 EMAC vendor config | none — SMI pins live on `mac_config` | `eth_esp32_emac_config_t` via `ETH_ESP32_EMAC_DEFAULT_CONFIG()` | same as 5.5 |
| SMI (MDC/MDIO) pins | `mac_config.smi_mdc_gpio_num` | `esp32_emac_cfg.smi_gpio.mdc_num` ✅ | `esp32_emac_config.smi_gpio.mdc_num` (identical) |
| MAC constructor | `esp_eth_mac_new_esp32(&mac_config)` (1 arg) | `esp_eth_mac_new_esp32(&esp32_emac_cfg, &mac_cfg)` (2 args) ✅ | same (2 args) |
| PHY driver | `esp_eth_phy_new_lan8720()` | `esp_eth_phy_new_lan87xx()` | **`esp_eth_phy_new_generic()`** (new) |
| RMII clock | Kconfig (`CLK_OUT` GPIO17 for Olimex) | in code per board ✅ — Olimex `EMAC_CLK_OUT`/GPIO17, WT32 `EMAC_CLK_EXT_IN`/GPIO0 | Kconfig-driven (`EMAC_CLK_EXT_IN`/`EMAC_CLK_OUT`) |
| netif default handlers | `esp_eth_set_default_handlers()` (removed in v5) | not called ✅ | not called |
| ETH link events | not handled | registers `ETH_EVENT`/`ESP_EVENT_ANY_ID` ✅ | registers `ETH_EVENT`/`ESP_EVENT_ANY_ID` for link up/down |
| PHY power GPIO | `gpio_pad_select_gpio()` + set level | `gpio_config()` struct ✅ | not done (official kit needs none) |
| Build glue | `register_component()` / `component.mk` | PlatformIO + glob `idf_component_register` | `idf_component_register(... PRIV_REQUIRES esp_netif esp_eth)` + `MINIMAL_BUILD ON` |

`src/main.c`'s Ethernet init now matches the 6.0 example on the structural points
(✅) and registers the `ETH_EVENT` link-state handler. The one optional delta left:

- **Generic PHY driver.** `esp_eth_phy_new_generic()` (added in IDF 5.4) drives any
  IEEE-802.3 PHY, including the LAN8720 — it's the new default. We use the
  exact-chip `esp_eth_phy_new_lan87xx()`, which is equally correct for both boards'
  LAN87xx PHYs, so switching is optional (do it only if moving to a
  different/unknown PHY).
- **Init order.** The 6.0 example installs the Ethernet driver *first*, then
  `esp_netif_init()`. `src/main.c` does netif first; both work, so left as-is.

### ESP32 RMII pins are FIXED (don't try to remap them)

The Kconfig only exposes RMII data-pin remapping under
`SOC_EMAC_USE_MULTI_IO_MUX`, which the **classic ESP32 does not have** (only
ESP32-P4 does). On both boards' ESP32 the RMII dataplane is hardwired:

`TXD0=19, TXD1=22, TX_EN=21, RXD0=25, RXD1=26, CRS_DV=27`, plus `MDC=23`,
`MDIO=18`. Only the clock source/mode/GPIO, PHY addr/reset, and PHY power pin are
configurable (and differ per board — see
[Supported boards](#supported-boards-compile-time-switch)). **These fixed pins are
exactly why DMX outputs must avoid them.** The 50 MHz RMII REF clock has strict
signal-integrity requirements.

> ⚠️ **A naive LAN8720/ESP32 default config matches Olimex, not WT32.** The common
> default outputs the clock (`EMAC_CLK_OUT`, GPIO17, PHY addr 0) — same as the
> Olimex ESP32-POE. The WT32-ETH01 is the opposite (external clock **in** on GPIO0,
> PHY addr 1). The firmware configures the clock **in code** per board, so it
> doesn't depend on any such default.

The cycle-counting DMX core (`xthal_get_ccount`, raw `0x3ff440xx` GPIO regs,
`vPortEnterCritical`) is ESP32-SoC-level and unaffected by any of this — it ports
as-is across all IDF versions.

### Version note

Project is pinned to **IDF 5.5** (`CONFIG_IDF_INIT_VERSION="5.5.0"`); the vendored
examples are **6.0.1**. The Ethernet APIs used here (nested `smi_gpio`, 2-arg MAC
ctor, generic PHY) exist in both, so the example is a valid guide. If actually
bumping to IDF 6.0, verify PlatformIO's `espressif32` platform ships an IDF-6.0
package first — the toolchain bump, not the Ethernet code, is the risk.

## Build / flash / monitor

PlatformIO wrapping ESP-IDF CMake ([platformio.ini](platformio.ini)). **Four envs**
= 2 boards × 2 variants; each passes a `BOARD_*` and `VARIANT_*` macro that
`src/main.c` `#ifdef`s on:

| Env | Board | Variant | Status |
|---|---|---|---|
| `olimex-rack` | esp32-poe | rack | deployed hardware |
| `olimex-mini` | esp32-poe | mini | deployed hardware |
| `wt32-rack` | wt32-eth01 | rack | secondary; pin map TBD |
| `wt32-mini` | wt32-eth01 | mini | secondary; pin map TBD |

```bash
pio run -e olimex-rack               # build (no -e builds all four envs)
pio run -e olimex-rack -t upload     # build + flash over USB serial
pio run -e olimex-rack -t monitor    # serial monitor (printf + ESP_LOGI)
pio run -t clean
pio run -e olimex-rack -t menuconfig # edit that env's sdkconfig
```

Each env gets its own generated `sdkconfig.<env>`, seeded by the shared
[sdkconfig.defaults](sdkconfig.defaults) (see below). `pio` is often not on `PATH`:
on this Linux box it lives at `~/.platformio/penv/bin/pio` (invoke with the full
path); on Windows the venv is `%USERPROFILE%\.platformio\penv\Scripts` — add it to
PATH or use the VS Code PlatformIO toolbar.

The build uses the chip's full **4 MB flash** (`CONFIG_ESPTOOLPY_FLASHSIZE="4MB"`
in [sdkconfig.defaults](sdkconfig.defaults)) with a **dual-slot OTA partition
table** ([partitions.csv](partitions.csv), selected via `board_build.partitions`
in [platformio.ini](platformio.ini)) — see [Firmware update (OTA)](#firmware-update-ota).
The firmware uses ~20% of a ~1.94 MB app slot. (Earlier single-env builds capped
flash at 2 MB and printed a benign `Flash memory size mismatch` warning; that is
gone now that the config matches the real 4 MB.)

## sdkconfig: what DMX timing requires

The DMX bit-bang only works with the right CPU-frequency and watchdog settings.
These are **board-independent**, so they live in a shared, git-tracked
[sdkconfig.defaults](sdkconfig.defaults) that seeds **every** env's generated
`sdkconfig.<env>` on first build:

| Setting (in `sdkconfig.defaults`) | Value | Why it matters |
|---|---|---|
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` | **y (240 MHz)** | 960 cycles/bit only equals 250 kbaud at 240 MHz |
| `CONFIG_ESP_INT_WDT_CHECK_CPU1` | **n** | core 1 sits in a critical section forever; the int WDT on CPU1 would panic |
| `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1` | **n** | core 1 idle task never runs; the task WDT would trip |

(`FREERTOS_UNICORE` off and `FREERTOS_HZ=100` are the ESP32 defaults already.)
These match the original working OLIMEX config. `sdkconfig.defaults` only **seeds**
a fresh config — if you need to change one of these later, do it via `menuconfig`
(it edits the per-env `sdkconfig.<env>`), or delete the generated file to re-seed.

`sdkconfig.defaults` also carries the **OTA foundation** settings, which are
likewise board-independent: `CONFIG_ESPTOOLPY_FLASHSIZE="4MB"` and
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. The partition *table* itself is **not**
set here — PlatformIO overrides the sdkconfig partition choice, so it is selected
by `board_build.partitions = partitions.csv` in [platformio.ini](platformio.ini)
(a sdkconfig `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` is silently ignored under the
PlatformIO ESP-IDF build). See [Firmware update (OTA)](#firmware-update-ota).

Finally it enables **FreeRTOS run-time stats** for the core-0 CPU-load metric
(**R29**): `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` (which auto-selects
`FREERTOS_USE_TRACE_FACILITY`) with `CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y`.
Board-independent and harmless to the DMX core (the bit-bang bypasses the scheduler).
**Note:** because `sdkconfig.defaults` only *seeds*, adding these meant deleting the
already-generated `sdkconfig.olimex-*` files so they re-seed — existing checkouts
must do the same (or set the options via `menuconfig`) to pick them up.

## Firmware update (OTA)

The node is updated over the network (goal **G6**; reqs **R25**/**R26**, todo
**T10**/**T11**). The web-UI upload path (**R26**) is implemented: build the image,
then upload `firmware.bin` through the web page. The optional ArduinoOTA/espota
responder (**R25**) is still to come.

**Flash & partitions.** Uses the real 4 MB flash with a dual-slot OTA layout
([partitions.csv](partitions.csv)): `ota_0`/`ota_1` (~1.94 MB each) + `otadata`, no
factory/phy_init partition. The node runs one slot and writes the next image into
the other; `otadata` then selects the new slot on reboot. First boot (empty
`otadata`) runs `ota_0`. The table is selected by `board_build.partitions` in
[platformio.ini](platformio.ini) (PlatformIO ignores the sdkconfig partition
filename — see the sdkconfig section above).

**Rollback.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A freshly OTA-written
image boots `PENDING_VERIFY`; if it crashes before being confirmed, the bootloader
reverts to the previous slot. `app_main` confirms the running image
(`esp_ota_mark_app_valid_cancel_rollback`) once Ethernet and the DMX/network tasks
have started — end of `app_main` in [src/main.c](src/main.c). USB-flashed images
are `UNDEFINED` and left untouched (no rollback), so a normal `pio run -t upload`
is unaffected.

> **Why DMX is parked through boot (the confirm is a flash op).**
> `esp_ota_get_running_partition()` / `esp_ota_mark_app_valid_cancel_rollback()`
> touch flash, which disables the flash cache on **both** cores and needs core 1 to
> answer a cross-core cache-stop. `dmx_task` holds core 1 inside
> `vPortEnterCritical()` with interrupts off **forever** while live, so it can never
> answer — running the confirm with DMX live **deadlocks core 0** (the task WDT then
> trips on `IDLE0`). That's why `stopFlag` starts set and `startDMX()` runs *after*
> the confirm: the whole boot, this confirm included, runs with the generator parked
> out of its critical section. The same applies to the future `esp_ota_begin/write/
> end` path — it must run under the `stopDMX()`/`startDMX()` handshake.

**Web-UI upload (R26 / T11, done).** `ota_post_handler` serves `POST /api/ota`. The
browser streams the raw `firmware.bin` as the octet-stream request body; the handler
picks the inactive slot (`esp_ota_get_next_update_partition`), then
`esp_ota_begin` → `esp_ota_write` (looping over `httpd_req_recv` chunks) →
`esp_ota_end` (validates image magic/size/hash) → `esp_ota_set_boot_partition` →
`esp_restart`. The new image boots `PENDING_VERIFY` and the `app_main` confirm above
makes it permanent; an image that fails to come up rolls back. The whole handler runs
under `stopDMX()` (DMX is **fully stopped for the entire update**, by decision — done
between shows), so the flash writes never race the core-1 critical section; on any
error it `esp_ota_abort`s and calls `startDMX()`, and the success path reboots. The
upload control + XHR progress bar are in [src/index.html](src/index.html).

> **Image file:** upload `.pio/build/<env>/firmware.bin` (the **app** image), not the
> merged bootloader+partitions image. That is what `esp_ota_write` expects.

**Still to do (T12).** The optional ArduinoOTA/espota responder for the PlatformIO
Upload button (**R25**) — a separate front-end on the same `esp_ota_*` engine.

## Task / core layout

As implemented in `app_main` (unchanged from the original):

| Task | Core | Priority | Role |
|---|---|---|---|
| `dmx_task` | **1** | `configMAX_PRIORITIES-1` | bit-bangs DMX, holds critical section (owns core 1) |
| `eth_task` | 0 | idle+2 | UDP Art-Net in (port **6454**) |
| `stats_task` | 0 | idle+1 | samples core-0 CPU load once a second (R29) |
| `httpd` | **0** | default (5) | web config UI (port **80**); started by `start_webserver()` with `config.core_id = 0` |

The network tasks are all on core 0 so `dmx_task` can monopolize core 1. The
`esp_http_server` task **must** stay on core 0 (`config.core_id = 0`) — its default
affinity is `tskNO_AFFINITY`, which could otherwise land it on core 1. Don't pin
anything else to core 1.

## Web config interface (R23 / T8)

`start_webserver()` in [src/main.c](src/main.c) runs an `esp_http_server` on **port
80**, pinned to core 0. It is the **sole** runtime config interface and hosts the
OTA upload endpoint (**R26**). The raw TCP CLI it replaced was **removed**
(2026-06-26, R4/R17 retired — see [reqs/R23.md](reqs/R23.md)).

| Route | Method | Effect |
|---|---|---|
| `/` | GET | serves the embedded config page (`index_html`) |
| `/api/state` | GET | flat JSON: fw/board/variant, `name`, `patch[7]`, `sync`, `sync_addr`, `num_chan`, live `refresh`, live `pps`, live `load0` |
| `/api/config` | POST | query params `patch=a,b,…,g` · `sync=0\|1` · `sync_addr=n` · `num_chan=n` (any subset); applies + returns fresh state |
| `/api/ota` | POST | raw firmware image (octet-stream body) → inactive OTA slot → reboot; see [Firmware update (OTA)](#firmware-update-ota) |

`/api/config` accepts the same settings the old TCP CLI did — `patch` (per-output
universe), `sync`, `sync_addr`, `num_chan` — and applies them through the
`save_dmx_patch()` / `save_sync_state()` path, inheriting the `stopDMX()` → NVS →
`startDMX()` handshake so it never mutates live DMX state.

### Live monitoring graphs (R28 / T13)

`/api/state` also reports `pps` — **Art-Net packets per second**. `eth_task`
increments a counter on every datagram that passes the `"Art-Net"` header check
(all opcodes), and once `esp_timer_get_time()` shows >1 s since the last window it
publishes the count into the `artnet_pps` global and restarts the window. The
window check runs every loop iteration, and the UDP socket has a 250 ms
`SO_RCVTIMEO`, so the rate **decays to 0 when Art-Net stops** (rather than holding
the last value) — that's what makes it useful for spotting "no packets reaching the
node" while debugging a rig. The idle `recvfrom` then returns `-1` each tick, which
is why `recv_len` is signed and guarded (closed bug B7).

`/api/state` also reports `load0` — **core-0 CPU utilization** (0-100 %, R29).
`stats_task` (core 0) takes a `uxTaskGetSystemState()` snapshot once a second, reads
the core-0 idle task's run-time counter (matched by handle), and computes
`load0 = 100 * (1 - idle_busy / wall)` over the 1 s window. This needs FreeRTOS
run-time stats — see the sdkconfig section. **Core 1 is intentionally not measured**:
`dmx_task` owns it via a forever critical section (R9/R10), so its idle task never
runs and any scheduler-based figure there would be meaningless (it is 100 %
`dmx_task` by definition). The run-time-stats accounting lives entirely in the
scheduler and does not perturb the cycle-counted DMX core.

The web page polls `/api/state` ~once a second, keeps the last 60 samples of `pps`,
`refresh` and `load0` client-side, and draws three `<canvas>` sparklines (newest
sample at the right, 0-baselined; traffic/refresh auto-scale to the window max, load
is fixed 0-100 %). **No history is stored on the ESP** — the firmware exposes only
the instantaneous values; the browser builds the 1-minute window. No libraries/CDNs
(R7/R27).

### How the page is embedded

The UI source is [src/index.html](src/index.html). A pre-build script
([gen_web_assets.py](gen_web_assets.py), wired via `extra_scripts` in
[platformio.ini](platformio.ini)) regenerates `src/web_assets.h` — a
null-terminated `index_html[]` C array — before every build, and `main.c` serves it
with `HTTPD_RESP_USE_STRLEN`. `web_assets.h` is git-ignored (generated). This
indirection exists because PlatformIO's SCons build does **not** link IDF's
`EMBED_TXTFILES` / `board_build.embed_txtfiles` cleanly on this platform version
(the `.S` is generated but never linked → `undefined reference
to _binary_index_html_start`). The script approach also extends to future assets
(e.g. an OTA page): emit more arrays from the same source dir.

## Native Art-Net configuration & query (R22)

Standard lighting controllers can discover and configure the node over Art-Net,
not just via the web UI. Handled in `eth_task`'s opcode dispatch
([src/main.c](src/main.c)), reusing the same `stopDMX()` → NVS → `startDMX()`
handshake as `/api/config`.

**Port-Address model (Art-Net 4 per-port binding).** A 15-bit Port-Address =
`NetSwitch(7) : SubSwitch(4) : Sw(4)`. A single ArtPollReply/ArtAddress carries
one Net + one Sub but four Sw nibbles, so the legacy scheme could only describe 4
ports sharing one block of 16 universes. Art-Net 4 instead encodes **one universe
per ArtPollReply** (`NumPortsLo=1`), differentiated by `BindIndex` (1..N) + `BindIp`
(see [docs/art-net.pdf](docs/art-net.pdf) p.149, p.4486-4489). So the node sends
**NUM_OUT (7) ArtPollReply packets**, BindIndex 1..7, each advertising one output's
fully independent universe — every output can have any universe.

- **ArtPoll (`0x2000`)** → `send_artpollreply()` emits the 7 per-port replies
  (unicast to the poller). Each fills NetSwitch/SubSwitch/SwOut from `DMX_patch[k]`,
  `PortTypes[0]=0x80` (output/DMX512), `GoodOutputA[0]=0x80`, the ShortName/LongName,
  MAC (`esp_read_mac`), BindIp/BindIndex, and Status1/Status2. Reply length 239
  (≥ the 207-byte minimum).
- **ArtAddress (`0x6000`)** → `handle_artaddress()`. `BindIndex` selects the output
  (`idx = bind<=1 ? 0 : bind-1`). The new universe is rebuilt from the current one,
  replacing only the Net/Sub/Sw sub-fields whose source byte has **bit7 set** (spec:
  "ignored unless bit 7 is high"), so a partial program never zeroes a universe.
  PortName (offset 14) / LongName (offset 32) are applied if their first byte is
  non-null. Changes persist (`save_dmx_patch` + `update_dmx_ptr`, `save_node_name`),
  then the node replies with ArtPollReply. **Any Art-Net patch change also resets
  sync mode and `num_chan` to defaults** (free-run, 512 channels) — those can't be
  set over Art-Net, so resetting them keeps an Art-Net-only configured node from
  silently retaining stale web-UI values the user can't see from a controller. The `Command` byte (LED/merge/failsafe
  ops) is accepted but not acted on (output-only node). Guarded by `recv_len >= 107`.
- **ArtInput (`0x7000`)** is **not** handled — the node has no DMX inputs. The
  pre-R22 firmware mis-used `0x6000`/`0x7000` as extra ArtPoll triggers; removed.

## Constants & wire format

- Art-Net UDP port **6454** (`PORT`); web UI on port **80**; socket buffer **1024**
  (`BUFLEN`).
- DMX timing: `BREAK` 30, `MAB` 10 bit-times; 11 bit-times/channel; 960 cycles/bit.
- Art-Net opcodes (little-endian on the wire — note byte-swapped reads like
  `(buf[9]<<8)|buf[8]`; `#define`d as `OP_*` in [src/main.c](src/main.c)): ArtDMX
  `0x5000`; ArtPoll `0x2000` → ArtPollReply `0x2100`; ArtAddress `0x6000`
  (remote config, R22); ArtInput `0x7000` (unhandled — output-only node). ArtDMX
  payload: length at bytes 16–17, data from byte 18; universe (port-address) at
  bytes 14–15. (The pre-R22 firmware mis-used `0x6000`/`0x7000` as extra ArtPoll
  triggers; that quirk is gone.)
- NVS namespace `"storage"`. Keys: `DMXPATCH` (blob), `SYNC_STATE` (u8),
  `SYNC_ADDR` (u16), `NUM_CHAN` (u16), `SHORTNAME` (str), `LONGNAME` (str).

## Toolchain facts

- Target project: ESP-IDF **v5.5.0**, ESP32 (dual-core Xtensa LX6), 4 MB flash,
  dual-slot OTA partition table ([partitions.csv](partitions.csv)).
- The OLIMEX original it was ported from used old ESP-IDF (~v4.0) — `component.mk`
  + legacy CMake `register_component()`.

## Conventions & gotchas

- **No external dependencies.** ESP-IDF / LwIP / FreeRTOS APIs only.
- **Style:** terse C, tabs, `uint_fast*` types, `printf` + `ESP_LOGI` (TAG is
  `"ArtNetNode"` in `src/main.c`). Match surrounding style.
- **Endianness:** Art-Net is little-endian; the manual byte assembly is
  intentional — keep it consistent.
- **Node name:** `node_short_name`/`node_long_name` seed from `VARIANT_NAME`
  (so a `mini` build no longer announces "Rack" — B12 fixed), are loaded from NVS
  (`SHORTNAME`/`LONGNAME`), reported in ArtPollReply + `/api/state`, and settable
  over Art-Net via ArtAddress (R22).
- **`xthal_get_ccount()`** in `dmx_task` is the original's cycle counter; it still
  resolves on IDF v5 via the Xtensa FreeRTOS port. If a build ever can't find it,
  swap to `esp_cpu_get_cycle_count()` (`esp_cpu.h`) — same value.
- **Don't hand-edit the generated `sdkconfig.<env>` files** — use `menuconfig` (per
  env). Keep DMX-timing settings in sync across both boards' configs. **Don't edit
  anything under `.pio/`** (build output, git-ignored).
- **Concurrency:** DMX state shared with the network tasks (`DMXbuf`, `DMX_patch`,
  `synchronize`, `trigger`, `num_chan`) is coordinated via `stopFlag`, not locks.
  Preserve the stop/start handshake around any reconfiguration; never write
  `DMXbuf`/patch tables while the generator is live.
- Live-lighting device: prefer changes that preserve output stability, bounded
  latency, and bounds-checked buffer math (the ArtDMX path indexes shared buffers
  directly).
