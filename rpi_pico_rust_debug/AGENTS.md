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

Tests run from the **repo root**, not here:

```sh
cargo test -p rp2040_temp
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
  debugging exercises before "fixing" it. **Do not "fix" it.** It has been
  fixed by mistake once (commit `3e48f14`), which silently broke the whole
  exercise while four doc sites went on promising a panic. If it ever is
  fixed on purpose, `README.md`'s step 1 and step 2.4, the comment above the
  line, the `HISTORY_LEN` doc comment, and this bullet all have to change
  with it.
- **`SAMPLE_COUNT` is not dead code.** It's a `static` mirror of `index`,
  and it exists only so the GDB watchpoint exercise in `README.md` has a
  named symbol at a fixed address to watch — `index` itself is a local of an
  `async fn` that neither `probe-rs` nor GDB can resolve.
- **`build.rs` embeds `memory.x` and sets the `-Tlink-rp.x` / `-Tdefmt.x`
  linker args** — that's the current `embassy-rp` convention (checked against
  its own `examples/rp` at time of writing), not something specific to this
  project. Keep this project's `Cargo.toml`, `build.rs`, and `memory.x` in
  sync with upstream `embassy-rp` examples when bumping `embassy-*` versions,
  since their linker/feature requirements have changed across versions.
  In particular `memory.x` is `MEMORY`-only on purpose: `embassy-rp`'s own
  `link-rp.x.in` already defines the `.boot2` output section, and the crate
  places the bootloader itself via `#[link_section = ".boot2"]`. A local
  `SECTIONS` block duplicates it, and the `EXTERN(BOOT2_FIRMWARE)` that used
  to accompany it names a symbol no current `embassy-rp` defines (it's from
  the older `rp2040-boot2` convention) — harmless under `rust-lld`, a link
  error under a stricter linker.
- **`rp2040_temp/` is split out to be testable, and must stay dependency-free.**
  It's a `no_std` leaf crate holding the ADC→millidegrees conversion, and it is
  a member of the **repo-root** workspace (with `exclude = ["rp2040_temp"]`
  here to stop this workspace claiming it as a path dep). That's the whole
  trick: `cargo test` inside this directory picks up `.cargo/config.toml`'s
  `[build] target = thumbv6m-none-eabi` and dies with `can't find crate for
  'test'`, because libtest needs `std`. From the root the pin doesn't apply.
  Adding any embedded dependency to it — embassy, cortex-m, defmt — makes it
  unbuildable for the host and breaks `cargo test` at the root for the whole
  repo. Put such code in `src/main.rs` instead.
- **Formatting reaches this crate through a second `cargo fmt`** in the root
  `pixi.toml`'s `fmt-rust` task, passing `--manifest-path` explicitly. The
  repo-wide `cargo fmt --all` only covers root-workspace members, and this
  crate deliberately isn't one, so without that it silently goes unformatted.

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
