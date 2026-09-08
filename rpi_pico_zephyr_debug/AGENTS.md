# rpi_pico_zephyr_debug

A minimal C++/Zephyr firmware for the Raspberry Pi Pico W (RP2040), built to
exercise a debugger — breakpoints, watchpoints, and a fatal-error backtrace —
over SWD with a Raspberry Pi Debug Probe and OpenOCD, rather than to do anything
useful on its own. Wiring and the full debugging walkthrough are in
[`README.md`](README.md).

This is a *freestanding* Zephyr application built against the tree in
`~/zephyrproject`, so `ZEPHYR_BASE` must be exported before `west build`:

```sh
export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -b rpi_pico/rp2040/w -p auto .
west flash            # OpenOCD + cmsis-dap over SWD, no extra args needed
west debug            # OpenOCD + GDB: breakpoints, watchpoints, backtraces
west build -t menuconfig
```

Console (Zephyr shell + log) is `uart0` at 115200, bridged by the probe's UART
port — `pyserial-miniterm /dev/ttyACM0 115200`.

Host tests run on `native_sim`, not on the target:

```sh
west build -b native_sim -p auto -d build_test tests/temp_convert && ./build_test/zephyr/zephyr.exe
# or: west twister -p native_sim -T tests
```

## Must-knows

- **The bug in `src/main.cpp` is deliberate**: `sample_index` increments without
  wrapping, so `history[sample_index]` walks off the end after `HISTORY_LEN`
  (8) samples. That's the whole point — see [`README.md`](README.md)'s "What to
  try" before "fixing" it. **Do not "fix" it.** If it's ever fixed on purpose,
  `README.md`'s bug description and "Fixing the bug" section, the comment block
  around the line, and this bullet all have to change together.

- **`__ASSERT` is what makes the bug observable, and is not optional.** C++ does
  not bounds-check the array write — the out-of-bounds store is silent UB. The
  `__ASSERT(sample_index < HISTORY_LEN, ...)` above it (needs `CONFIG_ASSERT=y`)
  is the stand-in for Rust's implicit bounds check: it turns the overflow into a
  deterministic fatal error with file:line and a backtrace. Removing it doesn't
  "fix" anything — it just hides the bug behind memory corruption. The C++/Rust
  safety contrast is a documented teaching point, not an accident.

- **`sample_count` is not dead code.** It's a file-scope `volatile` mirror of
  `sample_index`, kept as a clean, named, fixed-address target for the GDB
  watchpoint exercise. `sample_index` itself is a plain file-scope static GDB
  could watch directly; the mirror exists so the exercise has a symbol whose only
  job is being watched. `volatile` stops `-Og` folding the store away.

- **`src/temp_convert.{hpp,cpp}` must stay free of Zephyr headers.** That's the
  whole reason it's split out: the identical TU is compiled both for the ARM
  firmware and for the `native_sim` host test under `tests/temp_convert/` (which
  `#include`s it via a relative path in its `CMakeLists.txt`). Pulling in a
  Zephyr header would break the host build and the tests. Put anything that
  needs Zephyr in `src/main.cpp` instead.

- **The host test uses C headers (`<math.h>`, `<stdint.h>`), not `<cmath>`.**
  Zephyr builds C++ against its *minimal* libc++ with `-nostdinc++`, which ships
  `<cstdint>` but not `<cmath>`/`<cstdlib>` — so `std::fabs`/`std::lround`/
  `std::abs` don't resolve. `tests/temp_convert/src/main.cpp` uses the C math
  functions and a hand-rolled `abs_i32` for that reason.

- **The reading is synthetic on purpose.** `next_raw()` fabricates the 12-bit
  value in software rather than sampling the onboard temperature sensor. The ADC
  is skipped deliberately — the exercise is the debugger and the bug, and a
  deterministic source keeps the walkthrough reproducible. It is also not free:
  the stock Zephyr `adc_rpi_pico` driver never sets the `TS_EN` bit, so channel 4
  would need a manual power-on. If you ever want a real reading, that's the catch.

- **The onboard LED is intentionally unused.** On a Pico W it's behind the CYW43
  wireless chip, not a GPIO. The heartbeat is an *external* LED on GP15, declared
  under `zephyr,user` in `app.overlay` and picked up with `GPIO_DT_SPEC_GET_OR`
  (so it's skipped cleanly when absent). Don't "restore" the onboard LED without
  pulling in the CYW43 stack properly.

- **The debug build settings are load-bearing, not boilerplate.**
  `CONFIG_DEBUG_OPTIMIZATIONS` (-Og) is what keeps breakpoints on the right
  source line and backtraces readable; at the default -O2 the loop is inlined
  and reordered past recognition. `CONFIG_EXTRA_EXCEPTION_INFO` is what enables
  `arch_stack_walk()` on Cortex-M (it gates `ARCH_HAS_STACKWALK`), without which
  `CONFIG_EXCEPTION_STACK_TRACE` prints no backtrace. `CONFIG_DEBUG_THREAD_INFO`
  is what makes OpenOCD/GDB thread-aware. `CONFIG_DEBUG_COREDUMP` (logging
  backend) streams a post-mortem dump over the console on a fatal error;
  `MEMORY_DUMP_LINKER_RAM` (not the default `MEMORY_DUMP_MIN`) is deliberate --
  it dumps all RAM so `history`/`sample_index` are inspectable in the off-line
  GDB replay, which is the point of README.md exercise 7.

- **`CMakeLists.txt`'s `app_set_runner_args` macro is what makes `west debug`
  and `west attach` work — don't remove it, and keep it above
  `find_package(Zephyr)`.** With `CONFIG_DEBUG_THREAD_INFO=y` the OpenOCD runner
  appends `$_TARGETNAME configure -rtos Zephyr`, but RP2040's `target/rp2040.cfg`
  names its cores `_TARGETNAME_0`/`_TARGETNAME_1` and never defines the singular
  `_TARGETNAME`; the undefined variable aborts OpenOCD init and GDB then times
  out. The macro passes `--target-handle=_TARGETNAME_0` to fix it.
  `board_finalize_runner_args` only calls the macro if it's defined *before*
  `find_package` triggers board processing — hence its position — and it calls it
  once per registered runner (rpi_pico has six), so the macro self-guards with a
  GLOBAL property to append the arg exactly once. `west flash` never needed it;
  it doesn't emit the RTOS command. (Verify with `grep target-handle
  build/zephyr/runners.yaml` — expect exactly one.) Dropping
  `CONFIG_DEBUG_THREAD_INFO` would also dodge the crash, but at a cost: it's what
  `select`s `THREAD_NAME`/`THREAD_MONITOR` (see `prj.conf`), so removing it loses
  both the thread-aware `info threads` in GDB and the thread names the shell's
  `kernel threads` prints.

- **This directory carries its own `.clang-format`, copied from Zephyr's.**
  Without it the repo-wide `fmt` would reflow these files away from Zephyr's
  tabs-and-100-columns style. Do not delete it.

- **`build/` (and `build_test/`) hold symlinks into `~/zephyrproject`** — a fmt
  glob that follows them, rather than using `git ls-files` as the root `fmt-c`
  task does, would edit the Zephyr installation in place. Both are gitignored.

- **This area builds with west/CMake**, not Bazel, pixi, or cargo — so
  `ZEPHYR_BASE` must be exported before `west build`.

## Layout

- `CMakeLists.txt` — freestanding-app boilerplate; lists `src/main.cpp` and
  `src/temp_convert.cpp`.
- `prj.conf` — C++20, logging, the shell tier, GPIO, and the debug-build
  settings above.
- `app.overlay` — the optional heartbeat LED under `zephyr,user`.
- `src/main.cpp` — the sampling loop with the deliberate ring-buffer bug, and
  the `temp` shell command.
- `src/temp_convert.{hpp,cpp}` — the Zephyr-header-free conversion, shared with
  the host test.
- `tests/temp_convert/` — a `native_sim` ztest suite over `raw_to_millicelsius`,
  six tests checked against an independent floating-point reference.
- `60-openocd.rules` — udev rules so OpenOCD reaches the CMSIS-DAP probe without
  root (Linux); install to `/etc/udev/rules.d/`.
- `.clang-format` — Zephyr's, scoping this area's style away from the repo default.

`build/` is produced by west and is gitignored.
