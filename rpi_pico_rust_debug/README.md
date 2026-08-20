# rpi_pico_rust_debug

A minimal RP2040 project for exploring `probe-rs` debugging using only the
Pico's internal temperature sensor — no external wiring needed besides the
SWD link to the debug probe.

## Hardware

Target: Raspberry Pi Pico W (RP2040). Probe: the official Raspberry Pi Debug
Probe.

Connect the Debug Probe's "D" (SWD) port to the target's SWD header with the
supplied cable — it's a straight 1:1 connection, matched by signal name:

| Debug Probe | Target pin |
|---|---|
| SWCLK | SWCLK |
| GND   | GND   |
| SWDIO | SWDIO |

Power the target from its own USB cable, or from the probe's "T" (UART) port
if you want single-cable operation.

There's no LED demo here: on a Pico W the onboard LED is wired to the CYW43
wireless chip rather than a plain GPIO, so lighting it needs a PIO/SPI setup
and firmware blobs that would be a distraction from the debugging workflow
this project is about.

### Debug probe firmware

`probe-rs` requires CMSIS-DAP firmware >= 2.2.0 on the Debug Probe itself. If
`probe-rs list` / `probe-rs run` reports the firmware as outdated, put the
probe in bootloader mode (hold BOOTSEL while plugging it in), then copy the
latest `debugprobe.uf2` from the
[debugprobe releases page](https://github.com/raspberrypi/debugprobe/releases)
onto the `RPI-RP2` drive that appears.

## One-time setup

```bash
rustup target add thumbv6m-none-eabi
cargo install probe-rs-tools --locked
```

On Linux, copy [`69-probe-rs.rules`](69-probe-rs.rules) to
`/etc/udev/rules.d/` and reload udev (`sudo udevadm control --reload`) so the
probe is accessible without root.

Confirm the probe enumerates:

```bash
probe-rs list
```

You should see something like `Debug Probe (CMSIS-DAP)`.

## Build, flash, and stream logs

```bash
cargo run
```

This builds the firmware, flashes it over SWD, resets the target, and
streams `defmt` log output back over RTT directly in your terminal — no
separate flashing step or serial port required.

You should see `sample 0`, `sample 1`, ... logged every 500 ms, and after
`sample 7` the firmware will panic with an out-of-bounds array access
(that's the deliberate bug — see below).

## What to try

**1. Watch the panic and read the backtrace**
Just run `cargo run` and let it crash. `probe-rs` will print the panic
message and a backtrace pointing at the `history[index] = millicelsius;`
line in `src/main.rs`.

**2. Set a breakpoint**

```bash
cargo build
probe-rs attach --chip RP2040 target/thumbv6m-none-eabi/debug/rpi_pico_rust_debug
```

Then in the interactive session, set a breakpoint at the buggy line and
step through the last couple of iterations to watch `index` climb toward
`HISTORY_LEN`.

**3. Set a watchpoint on `index`**
Instead of stepping manually, set a watchpoint on the `index` variable and
let the program run freely — execution will stop the instant the value
changes, which is a much faster way to catch the moment things go wrong
than single-stepping.

**4. VS Code integration**
If you use VS Code, install the `probe-rs-debugger` (or `cortex-debug`)
extension and point it at this project's ELF output for a graphical
breakpoint/watch/variable-inspection workflow instead of the CLI.

**5. Read memory/registers directly**
With the target halted, try:

```bash
probe-rs read --chip RP2040 b32 <address> <count>
```

to peek at raw memory, or explore the ADC peripheral's registers to
confirm channel 4 (the temperature sensor) is selected as you'd expect
from `Channel::new_temp_sensor`.

## Fixing the bug

Once you've explored the debugging workflow, the fix is a one-line change:

```rust
index = (index + 1) % HISTORY_LEN;
```

Rebuild and confirm the firmware now runs indefinitely, logging
temperature samples without crashing.
