# CLAUDE.md

first things first. load ../LLMbootstrap/LLMbootstrap.md

@../LLMbootstrap/LLMbootstrap.md

@info.md

LLM-specific notes for the **ESP32 ArtNetNode** firmware (an Art-Net → DMX512 node
on an ESP32 Ethernet board). Everything else is defined in the project's own files
— do not duplicate it here:

- **[reqs.md](reqs.md)** (+ `reqs/<ID>.md`) — goals, requirements, bugs; the single
  source of truth for what's decided.
- **[todo.md](todo.md)** — the development plan (T1–T9).
- **[info.md](info.md)** — how the firmware works: architecture, DMX engine, board
  switch, ESP-IDF bring-up, build/flash, conventions & gotchas. Auto-loaded above.
- **[README.md](README.md)** — outward technical overview.
- **[docs/](docs/)** — bulky reference: [art-net.pdf](docs/art-net.pdf) (protocol),
  [ANSI E1.11 - 2024.pdf](docs/ANSI%20E1.11%20-%202024.pdf) (DMX512 standard).

## Working notes

- **Source-of-truth rule:** the DMX/Art-Net/TCP logic in `src/main.c` is the proven
  OLIMEX reference, ported line-for-line — preserve its behavior exactly when
  changing it. Only the Ethernet bring-up was modernized. See [info.md](info.md)
  for the full port history and the documented deltas.
- **Live-lighting device:** prefer changes that preserve output stability, bounded
  latency, and bounds-checked buffer math. Never write `DMXbuf`/patch tables while
  the generator is live — keep the `stopDMX()`/`startDMX()` handshake.
- **Style:** terse C, tabs, `uint_fast*`, `printf` + `ESP_LOGI` (TAG `"ArtNetNode"`).
  Match surrounding style. Prose: terse, no em-dashes.
- **Don't edit** generated `sdkconfig.<env>` files (use `menuconfig`) or anything
  under `.pio/` (build output, git-ignored).

## Git

- Default branch `main`; remote `origin` = `github.com/jannikmalter/ESP32_ArtNetNode`.
- Commit/push only when asked. Reference IDs in commits (e.g. `fix(B1): ...`).
- The orphaned `sdkconfig.wt32-eth01` (old single-env layout, wrong 160 MHz /
  watchdog settings) can be deleted — no env uses it.
