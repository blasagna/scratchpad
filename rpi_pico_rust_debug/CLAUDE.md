# rpi_pico_rust_debug

A minimal `no_std` embassy firmware for the Raspberry Pi Pico W (RP2040),
built to exercise `probe-rs` debugging (breakpoints, watchpoints, panic
backtraces) rather than to do anything useful on its own. Full hardware
setup, wiring, and the debugging walkthrough are in
[`README.md`](README.md).

It's its own standalone cargo workspace (`[workspace]` in `Cargo.toml`), not
a member of the repo-root one — it targets `thumbv6m-none-eabi` with a
`.cargo/config.toml` that pins `[build] target` and sets the `probe-rs run`
runner, which would break `cargo build --workspace` at the repo root for
every other (host-target) member.

```sh
cargo build          # debug profile, dev-friendly optimization
cargo build --release
cargo run             # build, flash over SWD, stream defmt/RTT logs live
```

`cargo run` needs the Raspberry Pi Debug Probe attached and wired to the
target's SWD header, and the target powered — see `README.md`.

## Must-knows

- **The onboard LED is intentionally unused.** On a Pico W it's wired to the
  CYW43 wireless chip, not a plain GPIO — driving it needs `cyw43` +
  `cyw43-pio` + firmware blobs, which is unrelated complexity for a debugging
  demo. Don't add it back without pulling that in properly (see
  `embassy-rs/embassy`'s `examples/rp/src/bin/wifi_blinky.rs` for what that
  actually takes).
- **The bug in `src/main.rs` is deliberate**: `index` increments without
  wrapping, so `history[index]` panics with an out-of-bounds access after
  `HISTORY_LEN` samples. That's the point — see `README.md` for the intended
  debugging exercises before "fixing" it.
- **`build.rs` embeds `memory.x` and sets the `-Tlink-rp.x` / `-Tdefmt.x`
  linker args** — that's the current `embassy-rp` convention (checked against
  its own `examples/rp` at time of writing), not something specific to this
  project. Keep this project's `Cargo.toml`, `build.rs`, and `memory.x` in
  sync with upstream `embassy-rp` examples when bumping `embassy-*` versions,
  since their linker/feature requirements have changed across versions.

## Layout

- `src/main.rs` — the async main task: ADC temperature sampling loop with the
  deliberate ring-buffer bug.
- `build.rs` — copies `memory.x` into `OUT_DIR` and emits the linker args
  `embassy-rp` needs.
- `memory.x` — RP2040 flash/RAM layout.
- `.cargo/config.toml` — pins the `thumbv6m-none-eabi` target and the
  `probe-rs run` runner.
- `69-probe-rs.rules` — udev rules so the debug probe is usable without root
  (Linux only); install to `/etc/udev/rules.d/`.
