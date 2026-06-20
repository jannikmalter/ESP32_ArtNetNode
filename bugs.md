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

### 6. ArtPollReply advertises IP 0.0.0.0
- **Where:** [src/main.c:647](src/main.c#L647)
- Copies `si_me.sin_addr.s_addr`, but `si_me` is bound to `INADDR_ANY` ([src/main.c:606](src/main.c#L606)) and never updated. The `IP_EVENT_ETH_GOT_IP` handler only logs — it never stores the real IP. Controllers relying on the reply's IP field (rather than the UDP source IP) will see 0.0.0.0.

### 7. `recvfrom` error not handled
- **Where:** [src/main.c:614-615](src/main.c#L614-L615)
- `recv_len` is `uint32_t`; a `-1` return becomes `0xFFFFFFFF`, which passes `>= 10` and processes a stale `ArtNetBuf`. If the socket itself failed to create (`s` is also an unchecked `uint32_t`, [src/main.c:596](src/main.c#L596)), this becomes a tight CPU-spinning busy loop.

---

## Low / latent / robustness

### 8. Unchecked allocations & return values
- `calloc` results (`DMXbuf` and the four task buffers) are never NULL-checked; `nvs_open`, `xTaskCreatePinnedToCore`, and `esp_netif_new` returns are ignored. Low probability, but a NULL `DMXbuf` would fault the DMX core immediately.

### 9. Signed shift in GPIO bitmask build
- **Where:** [src/main.c:194-195](src/main.c#L194-L195)
- `1 << GPIO_patch[j]` is a signed shift — fine for the current pins (≤ 17) but should be `1u <<`; would be UB at pin 31.

### 10. Read-modify-write on write-1-to-set registers
- **Where:** [src/main.c:273-274](src/main.c#L273-L274)
- `*GPIO_w1ts |= outputH` / `*GPIO_w1tc |= outputL` work (safe inside the critical section) but a plain `=` is the idiomatic, cheaper form for W1TS/W1TC registers.

### 11. `num_chan` only clamped in the TCP setter
- **Where:** [src/main.c:561-562](src/main.c#L561-L562)
- Clamped to 1–512 on the `NUM_CHAN` command, but not on NVS load — ties into bug #4.

### 12. Hardcoded "Rack" name regardless of variant
- **Where:** [src/main.c:490](src/main.c#L490), [src/main.c:654-655](src/main.c#L654-L655)
- A `mini` build still announces itself as "Rack" in the ArtPollReply and TCP banner. Already documented as a known wart in CLAUDE.md — not a regression. Fix by parameterizing on `VARIANT_NAME`.

### 13. Tick-vs-ms inconsistency in delays
- **Where:** `eth_task` `vTaskDelay(100)` ([src/main.c:613](src/main.c#L613)) vs `stopDMX` `vTaskDelay(100 / portTICK_PERIOD_MS)` ([src/main.c:298](src/main.c#L298))
- `vTaskDelay(100)` is 100 ticks = 1 s at `FREERTOS_HZ=100`, while the other is 100 ms. Harmless (just a reconfig pause) but inconsistent.

---

## Summary table

| # | Severity | Area | Bug | Masked in deployment? |
|---|---|---|---|---|
| 1 | High | Art-Net parse | Signed-`char` sign-extension corrupts length/universe — **FIXED** | Yes — full 512-ch frames only |
| 2 | High | Art-Net parse | No `DMXlength` bounds check before `memcpy` — **FIXED** | Partly — relies on well-formed senders |
| 3 | High | TCP parser | NULL `strtok` deref on malformed/short commands — **FIXED** | Yes — until a bad command is sent |
| 4 | High | NVS / boot | Uninitialized vars when NVS keys missing — **FIXED** | Yes — keys exist after first config |
| 5 | ~~Medium~~ | Concurrency | `update_dmx_ptr()` runs after DMX restart — **NOT A BUG (by design, memory-safe)** | n/a |
| 6 | Medium | Art-Net | ArtPollReply reports 0.0.0.0 | Depends on controller |
| 7 | Medium | Sockets | `recvfrom`/socket errors unhandled (`uint32_t`) | Yes — sockets normally succeed |
| 8 | Low | Robustness | Unchecked `calloc`/NVS/task-create returns | Yes |
| 9 | Low | DMX init | Signed `1 <<` shift | Yes — pins ≤ 17 |
| 10 | Low | DMX core | `|=` on W1TS/W1TC instead of `=` | Yes — works as-is |
| 11 | Low | NVS | `num_chan` not clamped on load | Tied to #4 |
| 12 | Low | Cosmetic | Hardcoded "Rack" name | Cosmetic |
| 13 | Low | Timing | `vTaskDelay` tick-vs-ms inconsistency | Harmless |
