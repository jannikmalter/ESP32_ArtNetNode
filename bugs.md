# Bugs — `src/main.c`

Audit of [src/main.c](src/main.c) against the committed baseline (post-cleanup). **No fixes applied** — this is a findings list only. Grouped by severity. "Masked" notes whether the bug is currently latent in the deployed Olimex setup and why.

Most of the serious findings (#1, #2, #7) share one root cause: **unvalidated, signed-`char` parsing of untrusted network packets in `eth_task`**. The TCP parser (#3) has the same "trusts input shape" flaw. #1–#3 were inherited verbatim from the OLIMEX original — they survive because the deployed traffic pattern never exercises them.

---

## High severity

### 1. Signed-`char` byte assembly corrupts length & universe → crash / dropped frames — **FIXED**
- **Where:** [src/main.c:592](src/main.c#L592) (`char *ArtNetBuf`), [src/main.c:622](src/main.c#L622), [src/main.c:634](src/main.c#L634)
- On Xtensa GCC, `char` is **signed**. The manual byte reassembly never casts to `uint8_t`:
  - `DMXlength = (ArtNetBuf[16] << 8) | ArtNetBuf[17]` — if the **Length low byte ≥ 0x80** (e.g. 200 or 384 channels), `ArtNetBuf[17]` sign-extends to `0xFFFFFF..`, making `DMXlength` ~4 billion → the `memcpy` at [src/main.c:635](src/main.c#L635) becomes a massive heap overflow → crash.
  - `portaddress = (ArtNetBuf[15] << 8) | ArtNetBuf[14]` — same issue; a SubUni byte ≥ 0x80 corrupts `portaddress`, so it matches no patch entry and that universe is **silently dropped**.
- **Masked because:** controllers almost certainly send full 512-channel frames (length lo-byte = 0x00) on low-numbered universes, so neither path triggers in the field. Breaks the moment a sender uses partial universes or sub-universe ≥ 128.
- **Fix applied:** each byte read in the three `eth_task` assembly expressions (`opcode`, `portaddress`, `DMXlength`) is now cast to `(uint8_t)` before the shift/OR, so no value sign-extends. Note this removes the bogus huge `DMXlength`, but a separate explicit upper bound is still missing — see **#2**.

### 2. No bounds check on `DMXlength` before `memcpy` — **FIXED**
- **Where:** [src/main.c:634-635](src/main.c#L634-L635)
- Even with correct parsing, nothing clamps `DMXlength` to ≤ 512, and nothing verifies `recv_len >= 18 + DMXlength`. A crafted or garbled packet (length > 512, or a truncated packet claiming 512) overflows `DMXbuf` (only `NUM_OUT*512` = 3584 bytes) or copies stale buffer garbage into channel data.
- Unauthenticated network input → memory corruption. Directly contradicts CLAUDE.md's "bounds-checked buffer math" priority.
- **Fix applied:** the ArtDMX branch is now guarded by `recv_len >= 18` (so the length/universe fields are actually present), and `DMXlength` is clamped to `512` and then to `recv_len - 18` before the `memcpy`. The `512` clamp runs first and is the hard safety net — it caps the copy at the slot size even if `recv_len` is bogus (see **#7**), so `DMXbuf` can no longer overflow; the `recv_len - 18` clamp additionally prevents copying stale bytes from a truncated packet.

### 3. TCP command parser dereferences NULL `strtok` results — **FIXED**
- **Where:** [src/main.c:519-560](src/main.c#L519-L560)
- A line starting with a space passes the `inputBuf[0] != 0` guard but `strtok` returns NULL → `strcmp(NULL, "PATCH")` at [src/main.c:521](src/main.c#L521) crashes.
- `SYNC` / `SYNC_ADDR` / `NUM_CHAN` with **no argument**: `strtok(NULL, " ")` returns NULL → `strcmp(NULL, "1")` ([src/main.c:538](src/main.c#L538)) / `atoi(NULL)` ([src/main.c:550](src/main.c#L550), [src/main.c:560](src/main.c#L560)) crash.
- `PATCH` is safe — its `while (token != NULL)` loop guards itself.
- Trivial remote DoS over TCP port 1337.
- **Fix applied:** the command dispatch now starts with an `if (token == NULL)` branch that ignores separator-only lines, and `SYNC` / `SYNC_ADDR` / `NUM_CHAN` each wrap their argument handling in `if (token != NULL)` so a missing argument is ignored rather than dereferenced. A command with no argument now simply re-echoes state with no change.

### 4. `load_sync_state` reads uninitialized stack on first boot — **FIXED**
- **Where:** [src/main.c:339-354](src/main.c#L339-L354)
- `sync_load` / `sync_addr_load` / `num_chan_load` are uninitialized locals. NVS return codes are **never checked**, and `nvs_get_*` leaves the destination untouched when a key is missing. On a fresh device or after `nvs_flash_erase()`, `synchronize` / `sync_addr` / `num_chan` get stack garbage → e.g. a garbage `num_chan` makes the frame-length math `(num_chan+1)*11+...` nonsensical (no output / near-zero refresh).
- Applies to every NVS getter in the file — return values are ignored throughout ([src/main.c:329-354](src/main.c#L329-L354)).
- **Fix applied:** the three locals are now initialized to safe defaults (`sync_load = 0`, `sync_addr_load = 0`, `num_chan_load = 512`) before the `nvs_get_*` calls. Since `nvs_get_*` only writes its destination on success, a missing key now leaves the default rather than stack garbage. `DMX_patch` in `load_dmx_patch` was already safe (it's a zero-initialized global, so a failed blob read leaves all-zero patches). Note: a *present but out-of-range* stored `num_chan` is still not clamped on load — that's **#11**.

---

## Medium severity

### 5. PATCH mutates live DMX state outside the stop/start window — **NOT A BUG (by design)**
- **Where:** [src/main.c:531-532](src/main.c#L531-L532)
- `save_dmx_patch()` calls `startDMX()` (clears `stopFlag`) before `update_dmx_ptr()` runs, so `update_dmx_ptr()` — which `memset`s all of `DMXbuf` and rewrites `DMX_repatch[]` — executes while `dmx_task` is live-reading those on core 1.
- **Verdict (reviewed with author):** intentional and safe. The stop/start window exists specifically to park `dmx_task` for the **NVS/flash** write (flash-cache disable vs. the core-1 critical section), *not* for the repatch. Running `update_dmx_ptr()` live is memory-safe: `DMX_repatch[i]` is always in `[0,6]` and written atomically, so `dmx_task`'s `DMXbuf[512*DMX_repatch[i] + curchan-1]` stays in bounds (max index 3583 < 3584) whichever value wins the race; the `memset` only yields old-or-zero byte values. The only effect is a transient frame on re-patch, which is acceptable for an inherently disruptive operation, and keeping it outside the stop window minimizes output interruption (the author's intent).
- **Optional polish (not required):** moving `update_dmx_ptr()` *inside* the stop window (stop → write NVS → repatch → start) would make the transition glitch-free at zero extra response-time cost, since the stop window already exists and the repatch is microseconds. Left as-is by choice.
- The `app_main` call order at [src/main.c:730](src/main.c#L730) is fine (tasks not started yet).

### 6. ArtPollReply advertises IP 0.0.0.0 — **FIXED**
- **Where:** [src/main.c:647](src/main.c#L647)
- Copies `si_me.sin_addr.s_addr`, but `si_me` is bound to `INADDR_ANY` ([src/main.c:606](src/main.c#L606)) and never updated. The `IP_EVENT_ETH_GOT_IP` handler only logs — it never stores the real IP. Controllers relying on the reply's IP field (rather than the UDP source IP) will see 0.0.0.0.
- **Not a regression:** `eth_task` and the got-IP handler are byte-for-byte identical to the old OLIMEX firmware, which had the same behavior. It went unnoticed because controllers reach the node via the UDP **source** IP, not this payload field.
- **Fix applied:** a `device_ip` global (network byte order) is set from `ip_info->ip.addr` in the `IP_EVENT_ETH_GOT_IP` handler, and the ArtPollReply now copies that into bytes 10–13 instead of the always-zero `si_me.sin_addr`. Reads 0.0.0.0 only until the link/DHCP assigns an address.

### 7. `recvfrom` error not handled
- **Where:** [src/main.c:614-615](src/main.c#L614-L615)
- `recv_len` is `uint32_t`; a `-1` return becomes `0xFFFFFFFF`, which passes `>= 10` and processes a stale `ArtNetBuf`. If the socket itself failed to create (`s` is also an unchecked `uint32_t`, [src/main.c:596](src/main.c#L596)), this becomes a tight CPU-spinning busy loop.

---

## Low / latent / robustness

### 8. Unchecked allocations & return values — **FIXED**
- `calloc` results (`DMXbuf` and the four task buffers) are never NULL-checked; `nvs_open`, `xTaskCreatePinnedToCore`, and `esp_netif_new` returns are ignored. Low probability, but a NULL `DMXbuf` would fault the DMX core immediately.
- **Fix applied:** the boot-critical allocations/inits (`DMXbuf`, `esp_netif_new`, the two task buffers in `tcp_task`/`eth_task`, and all three `xTaskCreatePinnedToCore` calls) now log `ESP_LOGE` and `esp_restart()` on failure — a restart loop is preferable to running with a NULL buffer or a missing DMX core. The four `nvs_open` calls are now checked: the **save** paths (`save_dmx_patch`/`save_sync_state`) call `startDMX()` before returning so a failed write never leaves DMX parked; the **load** paths keep their safe defaults (`load_dmx_patch` keeps the zero-init global patch, `load_sync_state` keeps `0/0/512`). Added `#include "esp_system.h"` for `esp_restart()`.

### 9. Signed shift in GPIO bitmask build — **FIXED**
- **Where:** [src/main.c:194-195](src/main.c#L194-L195)
- `1 << GPIO_patch[j]` is a signed shift — fine for the current pins (≤ 17) but should be `1u <<`; would be UB at pin 31.
- **Fix applied:** the bitmask build is now `1u << GPIO_patch[j]`, so the shift is on an `unsigned int` and well-defined for any pin 0–31. (The `outputH`/`outputL` build a few lines down also shifts by `GPIO_patch[i]`, but its left operand is already `0` or `1` and the pins are fixed ≤ 17 — left as-is to keep the fix scoped to the flagged expression.)

### 10. Read-modify-write on write-1-to-set registers — **FIXED**
- **Where:** [src/main.c:273-274](src/main.c#L273-L274)
- `*GPIO_w1ts |= outputH` / `*GPIO_w1tc |= outputL` work (safe inside the critical section) but a plain `=` is the idiomatic, cheaper form for W1TS/W1TC registers.
- **Fix applied:** both writes are now plain `=` (`*GPIO_w1ts = outputH; *GPIO_w1tc = outputL;`). W1TS/W1TC only act on the `1` bits written (a `0` is inert), and the masks are recomputed in full each bit-time, so the read in `|=` was pure overhead in the tightest loop of the firmware — dropping it removes a bus read per bit with identical behavior.

### 11. `num_chan` only clamped in the TCP setter
- **Where:** [src/main.c:561-562](src/main.c#L561-L562)
- Clamped to 1–512 on the `NUM_CHAN` command, but not on NVS load — ties into bug #4.

### 12. Hardcoded "Rack" name regardless of variant
- **Where:** [src/main.c:490](src/main.c#L490), [src/main.c:654-655](src/main.c#L654-L655)
- A `mini` build still announces itself as "Rack" in the ArtPollReply and TCP banner. Already documented as a known wart in CLAUDE.md — not a regression. Fix by parameterizing on `VARIANT_NAME`.

### 13. Tick-vs-ms inconsistency in delays — **FIXED**
- **Where:** `eth_task` `vTaskDelay(100)` ([src/main.c:613](src/main.c#L613)) vs `stopDMX` `vTaskDelay(100 / portTICK_PERIOD_MS)` ([src/main.c:298](src/main.c#L298))
- `vTaskDelay(100)` is 100 ticks = 1 s at `FREERTOS_HZ=100`, while the other is 100 ms. Harmless (just a reconfig pause) but inconsistent.
- **Fix applied:** `eth_task`'s reconfig-pause poll is now `vTaskDelay(1)` (one tick = 10 ms), matching `dmx_task`'s own stop-poll, so ingest resumes within ~10 ms of `startDMX()` instead of up to 1 s. `stopDMX` no longer uses a fixed delay at all — see **#14**.

### 14. `stopDMX` parked-state sync was a timing assumption, not a handshake — **FIXED**
- **Where:** `stopDMX` ([src/main.c:303-306](src/main.c#L303-L306)), `dmx_task` stop branch ([src/main.c:219-229](src/main.c#L219-L229)), `stopFlag`/`trigger` globals ([src/main.c:132](src/main.c#L132), [src/main.c:137](src/main.c#L137))
- `stopDMX` set `stopFlag = 1` then slept a fixed `vTaskDelay(100 ms)` and *assumed* `dmx_task` had left `vPortExitCritical` by then, before the caller (`tcp_task`) wrote NVS. Writing flash while `dmx_task` is still in its critical section (interrupts off, running from cache) risks a crash on the cache-disable/cross-core stall. Safe in practice only by margin (`dmx_task` checks `stopFlag` every ~4 µs, so 100 ms was ~25,000× headroom). Separately, `stopFlag`/`trigger` were plain `uint_fast8_t` shared across tasks — not `volatile`, so technically liable to be register-cached (worked only because the poll loops contain `vTaskDelay` the compiler can't see through).
- **Fix applied:** added a `volatile dmxStopped` ack flag that `dmx_task` raises right after `vPortExitCritical` and clears when it re-enters the critical section. `stopDMX` now clears `dmxStopped`, sets `stopFlag`, and polls `while (!dmxStopped) vTaskDelay(1)` (with a 50-tick failsafe so a wedged `dmx_task` can't hang the config interface) — a real wait-for-exit instead of a fixed guess, and it resumes within ~1 tick instead of always 100 ms. Clearing the ack *before* setting `stopFlag` ensures a fresh ack is awaited, closing the back-to-back-stop race. `stopFlag`, `dmxStopped`, and `trigger` are now `volatile`.

---

## Summary table

| # | Severity | Area | Bug | Masked in deployment? |
|---|---|---|---|---|
| 1 | High | Art-Net parse | Signed-`char` sign-extension corrupts length/universe — **FIXED** | Yes — full 512-ch frames only |
| 2 | High | Art-Net parse | No `DMXlength` bounds check before `memcpy` — **FIXED** | Partly — relies on well-formed senders |
| 3 | High | TCP parser | NULL `strtok` deref on malformed/short commands — **FIXED** | Yes — until a bad command is sent |
| 4 | High | NVS / boot | Uninitialized vars when NVS keys missing — **FIXED** | Yes — keys exist after first config |
| 5 | ~~Medium~~ | Concurrency | `update_dmx_ptr()` runs after DMX restart — **NOT A BUG (by design, memory-safe)** | n/a |
| 6 | Medium | Art-Net | ArtPollReply reports 0.0.0.0 — **FIXED** | Depends on controller |
| 7 | Medium | Sockets | `recvfrom`/socket errors unhandled (`uint32_t`) | Yes — sockets normally succeed |
| 8 | Low | Robustness | Unchecked `calloc`/NVS/task-create returns — **FIXED** | Yes |
| 9 | Low | DMX init | Signed `1 <<` shift — **FIXED** | Yes — pins ≤ 17 |
| 10 | Low | DMX core | `|=` on W1TS/W1TC instead of `=` — **FIXED** | Yes — works as-is |
| 11 | Low | NVS | `num_chan` not clamped on load | Tied to #4 |
| 12 | Low | Cosmetic | Hardcoded "Rack" name | Cosmetic |
| 13 | Low | Timing | `vTaskDelay` tick-vs-ms inconsistency — **FIXED** | Harmless |
| 14 | Low | Concurrency | `stopDMX` timing-assumption + non-`volatile` flags — **FIXED** | Yes — by timing margin |
