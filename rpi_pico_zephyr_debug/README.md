# rpi_pico_zephyr_debug

A minimal Raspberry Pi Pico W (RP2040) firmware whose only job is to be a
*target for a debugger* — breakpoints, watchpoints, and a fatal-error
backtrace — driven over SWD by a **Raspberry Pi Debug Probe** and **OpenOCD**,
built with **Zephyr** in **C++**.

The firmware samples a (synthetic) temperature every 500 ms, converts it to
millidegrees Celsius, and writes it into an 8-slot ring buffer whose index
**deliberately never wraps** — so after 8 samples it walks off the end of the
array. See [What to try](#what-to-try).

## Hardware

Target: Raspberry Pi Pico W (RP2040). Probe: the official Raspberry Pi Debug
Probe (or a second Pico running `debugprobe` firmware).

The Debug Probe carries two things down one USB cable: a **CMSIS-DAP** SWD
interface for flashing/debugging, and a **UART bridge** for the console. Wire
both:

| Debug Probe | Target pin | Purpose |
|---|---|---|
| SWCLK (D port) | SWCLK | SWD clock |
| GND (D port)   | GND   | SWD ground |
| SWDIO (D port) | SWDIO | SWD data |
| TX (T/UART port) | GP1 (physical pin 2) | console: probe TX → target RX |
| RX (T/UART port) | GP0 (physical pin 1) | console: probe RX → target TX |
| GND (T/UART port) | GND (e.g. pin 3) | console ground |

Power the target from its own USB cable. The console (Zephyr shell + log) runs
at **115200 8N1** on the RP2040's `uart0` (GP0 = TX, GP1 = RX), which the probe
bridges to a `/dev/ttyACM*` port on the host.

### External LED (optional)

The Pico W's onboard LED is behind the CYW43 wireless chip, not a plain GPIO,
so lighting it would drag in a PIO/SPI setup and firmware blobs — a distraction
from the debugging this project is about. An external LED on a free GPIO gives
the same 1 Hz heartbeat with none of that. Wire it and it toggles once per
sample; leave it out and the firmware runs identically without it.

| Component | Connects to |
|---|---|
| LED anode (long leg) | GP15 (physical pin 20), through a ~330 Ω resistor |
| LED cathode (short leg) | GND (e.g. physical pin 18) |

The pin is declared under `zephyr,user` in [`app.overlay`](app.overlay); change
it there, not in the source.

### Debug probe firmware

The Debug Probe must be running CMSIS-DAP firmware (it ships with it). If
`openocd` doesn't see it, put the probe in bootloader mode (hold BOOTSEL while
plugging it in) and copy the latest `debugprobe.uf2` from the
[debugprobe releases](https://github.com/raspberrypi/debugprobe/releases) onto
the `RPI-RP2` drive that appears.

## One-time setup

This is a **freestanding** Zephyr application built against the Zephyr tree in
`~/zephyrproject`, so `ZEPHYR_BASE` must be exported before every `west`
command:

```bash
export ZEPHYR_BASE=~/zephyrproject/zephyr
```

OpenOCD (≥ 0.12.0, for the `rp2040` target and `cmsis-dap` interface) must be
installed. On Linux, copy [`60-openocd.rules`](60-openocd.rules) to
`/etc/udev/rules.d/` and reload udev so the probe is usable without root:

```bash
sudo cp 60-openocd.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

Then unplug and replug the probe.

## Build, flash, and read the console

```bash
export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -b rpi_pico/rp2040/w -p auto .
west flash
```

The board's `board.cmake` defaults the runner to OpenOCD with the `cmsis-dap`
interface and the `rp2040` target over SWD, so `west flash` needs no extra
arguments. (To force a different adapter:
`west flash -- -DRPI_PICO_DEBUG_ADAPTER=<stem>`, matching an OpenOCD
`interface/<stem>.cfg`.)

Open the console in another terminal to watch it run:

```bash
pyserial-miniterm /dev/ttyACM0 115200
```

You'll see `sample 0`, `sample 1`, … logged every 500 ms, and after `sample 7`
the firmware stops with an assertion failure — the deliberate bug (see below).
The console is also a Zephyr shell: try `kernel threads`, `kernel stacks`, or
this app's own `temp` command, which dumps the ring buffer and the indices.

## Tests

The temperature conversion lives in its own Zephyr-header-free translation unit,
[`src/temp_convert.cpp`](src/temp_convert.cpp), so the exact same file compiles
for the firmware *and* for a host unit test on `native_sim` — the C++/Zephyr
analog of the Rust sibling's dependency-free `rp2040_temp` crate. Run it with
either:

```bash
west build -b native_sim -p auto -d build_test tests/temp_convert
./build_test/zephyr/zephyr.exe
```

or through Twister, which reads [`testcase.yaml`](tests/temp_convert/testcase.yaml):

```bash
west twister -p native_sim -T tests
```

Everything else here — the shell, GPIO, the fatal-error path — needs the real
chip (or at least the debugger), and is exercised by flashing, not by the host
tests.

## What to try

The bug is at the ring-buffer write in [`src/main.cpp`](src/main.cpp) (the
`history[sample_index] = millicelsius;` line, around line 139 — check the
current number before setting a breakpoint, since edits above it shift it).
`sample_index` increments without wrapping, so after `HISTORY_LEN` (8) samples
it's out of bounds.

In C++ that out-of-bounds write is *silent* undefined behaviour — unlike Rust,
where indexing is bounds-checked and panics on its own. The `__ASSERT` on the
line above stands in for that missing check: with `CONFIG_ASSERT=y` it turns the
overflow into a deterministic fatal error you can catch. (Remove it and you'd be
hunting silent memory corruption instead — which is the lesson.)

**1. Flash it and watch it fault**

Just `west flash` and watch the console. After `sample 7` the assertion fires:

```
ASSERTION FAIL [sample_index < 8] @ .../src/main.cpp:137
	history index 8 out of bounds
```

followed by a register dump and, thanks to `CONFIG_EXTRA_EXCEPTION_INFO` +
`CONFIG_EXCEPTION_STACK_TRACE`, a symbolic backtrace of function addresses. Feed
one of those addresses to `addr2line` (or just use `west debug` below) to place
it in the source.

**2. Set a breakpoint**

Launch OpenOCD + GDB with a single command (leave the console `miniterm`
running in its own terminal to watch `sample N` in parallel):

```bash
west debug
```

This flashes, resets, halts, and drops you at a `(gdb)` prompt. From there:

```
(gdb) break src/main.cpp:139
(gdb) continue
```

Each `continue` runs one more loop iteration until it stops at the write again.
While halted:

- `print sample_index` — **works here.** Unlike the Rust sibling (where `index`
  is a local of an async state machine the debugger can't resolve), this is a
  plain static, so GDB reads it by name. `print history` dumps the array.
- `bt` — the backtrace from the loop up through `main` and the kernel entry.
- `info threads` — the Zephyr thread list, courtesy of `CONFIG_DEBUG_THREAD_INFO`.
- `next` / `step` — single-step; watch `sample_index` climb toward 8.
- `continue` until it reaches 8 and the assert fires straight into GDB.

**3. Set a watchpoint on `sample_count`**

A hardware watchpoint stops the target the instant a value changes — much faster
than single-stepping. `sample_count` is a file-scope `volatile` mirror of
`sample_index`, kept precisely as a clean, named, fixed-address target for this:

```
(gdb) watch sample_count
(gdb) continue
```

Execution stops on each write to `sample_count`, printing its old and new
values. (You can also `watch sample_index` directly here — it's a static too;
`sample_count` just guarantees a stable symbol for the exercise.) The RP2040's
Cortex-M0+ has a limited number of hardware watchpoint comparators, so watch one
or two at a time.

**4. Attach to a running target**

To inspect firmware that's already running without reflashing:

```bash
west attach
```

Then `Ctrl-C` in GDB to halt, poke around (`bt`, `info threads`, `info registers`), and `continue`.

**5. VS Code**

For a graphical breakpoint/watch/variable/register workflow instead of the CLI,
use the Cortex-Debug extension — see
[Debugging in VS Code (Cortex-Debug)](#debugging-in-vs-code-cortex-debug) below
for a ready-to-use `launch.json`.

**6. Read memory and registers directly**

With the target halted in GDB:

```
(gdb) info registers
(gdb) x/8dw &history        # the 8 ring-buffer slots as signed words
(gdb) print &sample_count   # its address, if you'd rather watch the location
```

## Debugging in VS Code (Cortex-Debug)

For a graphical workflow — gutter breakpoints, the Variables/Watch panes, data
watchpoints, a call stack with Zephyr threads, and a peripheral-register viewer
— use the [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug)
extension driving the same OpenOCD + Pico probe setup as the CLI.

Install the **Cortex-Debug** extension. You already have the OpenOCD and
`arm-zephyr-eabi-gdb` it needs from the CLI setup above (they ship with the
Zephyr SDK).

Save the following as `.vscode/launch.json`. The build-output paths use
`${workspaceFolder}` assuming you open **this project directory**
(`rpi_pico_zephyr_debug/`) as the VS Code workspace folder; if you open the repo
root instead, change the two `build/zephyr/zephyr.elf` paths to
`rpi_pico_zephyr_debug/build/zephyr/zephyr.elf`. `${userHome}` expands to your
home directory.

```jsonc
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Pico W: Launch (OpenOCD)",
            "type": "cortex-debug",
            "request": "launch",
            "cwd": "${workspaceFolder}",
            "executable": "${workspaceFolder}/rpi_pico_zephyr_debug/build/zephyr/zephyr.elf",
            "servertype": "openocd",
            "serverpath": "${userHome}/zephyr-sdk-1.0.1/hosttools/sysroots/x86_64-pokysdk-linux/usr/bin/openocd",
            "gdbPath": "${userHome}/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb",
            "searchDir": [
                "${userHome}/zephyr-sdk-1.0.1/hosttools/sysroots/x86_64-pokysdk-linux/usr/share/openocd/scripts"
            ],
            "configFiles": [
                "interface/cmsis-dap.cfg",
                "target/rp2040.cfg"
            ],
            "openOCDLaunchCommands": [
                "adapter speed 2000",
                "rp2040.core0 configure -rtos Zephyr"
            ],
            "svdFile": "${userHome}/zephyrproject/modules/hal/rpi_pico/src/rp2040/hardware_regs/RP2040.svd",
            "runToEntryPoint": "main"
        },
        {
            "name": "Pico W: Attach (OpenOCD)",
            "type": "cortex-debug",
            "request": "attach",
            "cwd": "${workspaceFolder}",
            "executable": "${workspaceFolder}/rpi_pico_zephyr_debug/build/zephyr/zephyr.elf",
            "servertype": "openocd",
            "serverpath": "${userHome}/zephyr-sdk-1.0.1/hosttools/sysroots/x86_64-pokysdk-linux/usr/bin/openocd",
            "gdbPath": "${userHome}/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb",
            "searchDir": [
                "${userHome}/zephyr-sdk-1.0.1/hosttools/sysroots/x86_64-pokysdk-linux/usr/share/openocd/scripts"
            ],
            "configFiles": [
                "interface/cmsis-dap.cfg",
                "target/rp2040.cfg"
            ],
            "openOCDLaunchCommands": [
                "adapter speed 2000",
                "rp2040.core0 configure -rtos Zephyr"
            ],
            "svdFile": "${userHome}/zephyrproject/modules/hal/rpi_pico/src/rp2040/hardware_regs/RP2040.svd"
        }
    ]
}
```

`west build …` first, then open the **Run and Debug** panel (`Ctrl+Shift+D`),
pick a configuration, and press `F5`. **Launch** flashes `zephyr.elf`, resets,
and stops at `main`; **Attach** connects to firmware that's already running
(use the pause button to halt it). Then set breakpoints in the gutter, watch
`sample_index` / `sample_count` / `history` in the **Variables** and **Watch**
panes, right-click a variable for a **data watchpoint**, and open **Cortex
Peripherals** to inspect registers.

What the less-obvious fields do:

- **`serverpath` / `gdbPath`** — the Zephyr SDK's OpenOCD and Arm GDB.
  Cortex-Debug otherwise defaults to `arm-none-eabi-gdb` on `PATH`, which the
  SDK doesn't install, so both are given explicitly.
- **`configFiles` + `searchDir`** — the same `interface/cmsis-dap.cfg` and
  `target/rp2040.cfg` the CLI uses; `searchDir` points at the SDK's OpenOCD
  scripts so those names resolve.
- **`openOCDLaunchCommands`** — extra OpenOCD commands run after the config
  files. `rp2040.core0 configure -rtos Zephyr` turns on the Zephyr thread list
  in the call stack. Note it names the core target **directly** — this is the
  same RTOS-handle issue described under [Troubleshooting](#troubleshooting), but
  because you spell out `rp2040.core0` here there's no undefined-variable trap.
  Drop this line if you don't want thread awareness.
- **`svdFile`** — the RP2040 register map, which populates the **Cortex
  Peripherals** view so you can read the ADC/GPIO/etc. registers by name.
- **`runToEntryPoint`** — halt at `main` after flashing (Launch only).

## Troubleshooting

**`west debug` / `west attach` abort with `can't read "_TARGETNAME"` and GDB
then reports `could not connect: Connection timed out`.** This project already
works around it — the note here is so you understand the fix and can spot it on
other RP2040 boards.

With `CONFIG_DEBUG_THREAD_INFO=y` (which this project sets, for thread-aware
debugging) Zephyr's OpenOCD runner appends a command to turn on Zephyr RTOS
awareness:

```
$_TARGETNAME configure -rtos Zephyr
```

But RP2040 is a dual-core part, and its upstream `target/rp2040.cfg` names the
core targets `_TARGETNAME_0` / `_TARGETNAME_1` — it never defines the singular
`_TARGETNAME` the runner assumes by default. The undefined Tcl variable aborts
OpenOCD's init, so the GDB server never starts serving and the GDB client times
out. (`west flash` is unaffected — it doesn't emit the RTOS command.)

The fix is baked into [`CMakeLists.txt`](CMakeLists.txt), which points the runner
at the real handle via an `app_set_runner_args()` macro:

```cmake
board_runner_args(openocd "--target-handle=_TARGETNAME_0")
```

If you ever hit this on a project without that macro, pass the flag by hand:

```bash
west debug  --target-handle _TARGETNAME_0
west attach --target-handle _TARGETNAME_0
```

To sanity-check the probe itself independently of OpenOCD, `probe-rs list`
should show `Debug Probe (CMSIS-DAP)`; if it does, the probe, wiring, and
permissions are fine and any remaining failure is on the OpenOCD/GDB side.

## Fixing the bug

Once you've explored the workflow, the fix is one line in
[`src/main.cpp`](src/main.cpp) — wrap the index:

```cpp
sample_index = (sample_index + 1) % HISTORY_LEN;
```

Rebuild and reflash, and the firmware runs indefinitely, logging samples without
tripping the assert. (The commented fix is right there next to the bug.)
