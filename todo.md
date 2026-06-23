# Todos

Development plan. Each item references the [reqs.md](reqs.md) ID it advances;
priority in parentheses.

- [ ] **T1** — Implement link-local (AUTOIP) fallback when DHCP is unavailable
  (enable `CONFIG_LWIP_AUTOIP`, start on DHCP timeout). (R15, Medium)
- [ ] **T2** — Handle socket and `recvfrom` errors in `eth_task` (signed length,
  check returns). (B7, Medium)
- [ ] **T3** — Clamp `num_chan` to 1..512 on NVS load. (B11, Low)
- [ ] **T4** — Derive node name from `VARIANT_NAME` (banner + ArtPollReply).
  (B12, Low)
- [ ] **T5** — Define and verify WT32-ETH01 DMX pin maps on hardware (currently
  placeholders). (R19, R20, Medium when WT32 built)
- [ ] **T6** — Optional: move `update_dmx_ptr()` inside the stop/start window for
  glitch-free re-patch. (B5, Optional)
- [ ] **T7** — Implement native Art-Net config/query: ArtAddress (set + persist
  universe patch and node name), full ArtPollReply (per-port universes, name,
  status), optional ArtInput (0x7000); read [docs/art-net.pdf](docs/art-net.pdf).
  (R22, Medium)
- [ ] **T8** — Build minimal web UI: one `esp_http_server` config page (patch,
  sync, sync universe, channels/output + live refresh), reuse the NVS handshake,
  decide TCP CLI removal. (R23, Medium)
- [ ] **T9** — Rework sync: replace `trigger` with a "new data" flag set on
  sync-universe ArtDMX and on ArtSync (0x5200); in `dmx_task` wait for the flag and
  clear it at frame start (before send); keep the failsafe timeout. (R24, High)
