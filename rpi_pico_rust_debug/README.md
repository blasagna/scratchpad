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

There's no *onboard*-LED demo here: on a Pico W the onboard LED is wired to
the CYW43 wireless chip rather than a plain GPIO, so lighting it needs a
PIO/SPI setup and firmware blobs that would be a distraction from the
debugging workflow this project is about. An external LED on a free GPIO
sidesteps that entirely — see below.

### External LED (optional)

A plain GPIO-driven LED gives you a visual heartbeat alongside the RTT log
lines, and doubles as another thing to watch while single-stepping (does the
LED still toggle after you resume from a breakpoint?).

Wire it to any free GPIO — GP15 (physical pin 20) is a convenient one, away
from the ADC/SWD pins already in use:

| Component | Connects to |
|---|---|
| LED anode (long leg) | GP15 (physical pin 20), through a ~330Ω resistor |
| LED cathode (short leg) | GND (e.g. physical pin 18) |

The resistor can sit on either leg of the LED, as long as it's in series
between the GPIO and GND. Without it, driving the pin risks pushing more
current through the LED than either the LED or the RP2040's GPIO is rated
for.

In `src/main.rs`, drive it with `embassy_rp::gpio::{Level, Output}`:

```rust
use embassy_rp::gpio::{Level, Output};
```

```rust
let mut led = Output::new(p.PIN_15, Level::Low);
```

Then toggle it once per loop iteration, alongside the existing sampling
logic:

```rust
loop {
    let raw = adc.read(&mut temp_channel).await.unwrap();
    // ...
    led.toggle();
    Timer::after_millis(500).await;
}
```

At 500ms per iteration that's a 1Hz blink — slow enough to see by eye, and a
handy sanity check that the loop is still running normally when you're not
watching the RTT output.

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

Build first, then launch the interactive command-line debugger. `--launch`
flashes the binary, resets the target, and lets it start running
immediately — you don't need to build and flash separately:

```bash
cargo build
probe-rs debug --chip RP2040 --launch target/thumbv6m-none-eabi/debug/rpi_pico_rust_debug
```

You'll land in a `Debug Console>` prompt while the firmware is already
running freely and logging `sample N` lines from RTT. From here:

1. **Set the breakpoint** at the buggy line:

   ```
   break src/main.rs:58
   ```

   `probe-rs` prints the address it resolved (e.g. `Breakpoint set at
   0x10000426`). Since the firmware is already running, it hits the
   breakpoint on its own the next time the loop reaches that line — you
   should see `Halted on breakpoint (...) @<address>.` appear without
   typing anything else.

2. **Resume and re-hit it** with `c` (continue). Each `c` lets one more
   loop iteration run until it hits `src/main.rs:58` again; watch the
   `sample N` RTT lines between hits to track how many iterations have
   passed. Keep typing `c` through a few iterations.

3. **Inspect the call stack** with `bt` (backtrace) while halted — it
   prints the full frame list from the breakpoint up through the embassy
   executor and the reset vector, with file:line for each frame. Other
   useful commands while halted: `step` (single instruction step), `info
   break` (list active breakpoints), `clear src/main.rs:58` (remove one).

   Note: on this project (an `async fn main`, compiled by embassy into a
   generated state-machine "task"), `p <var>` and `info locals` currently
   can't resolve locals like `index` inside that frame — `probe-rs` reports
   `No variable named ... found for frame: "{async_fn#0}"`. This is a
   limitation of how `probe-rs` walks the DWARF info for async-fn state
   machines, not something specific to this bug. The RTT `sample N` log
   lines are the reliable way to know the current value of `index` while
   you step through breakpoint hits.

4. Once you've seen enough hits, either type `c` repeatedly until index
   reaches `HISTORY_LEN` and you see the same panic and backtrace from
   step 1 printed straight to the `Debug Console`, or type `quit` to
   detach.

**3. Set a watchpoint on `index`**
The interactive `probe-rs debug` console above doesn't expose hardware
watchpoints directly, but `probe-rs`'s GDB server does. Start it in one
terminal:

```bash
probe-rs gdb --chip RP2040
```

This starts a GDB remote server on `localhost:1337` (left running until you
`Ctrl-C` it). In another terminal, connect with an Arm GDB client (e.g.
`arm-none-eabi-gdb` or `gdb-multiarch` — install separately, it's not part
of `probe-rs-tools`):

```
gdb-multiarch target/thumbv6m-none-eabi/debug/rpi_pico_rust_debug
(gdb) target remote localhost:1337
(gdb) watch index
(gdb) continue
```

Letting the program run freely and stopping the instant `index` changes is
a much faster way to catch the moment things go wrong than single-stepping
or repeatedly hitting a line breakpoint.

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
