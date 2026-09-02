# feather_sense_zephyr

A Zephyr RTOS application for the Adafruit Feather Bluefruit Sense (nRF52840):
IMU, magnetometer, humidity/temperature, light, battery and button sampled on
one I²C bus and streamed to a host over both USB CDC and BLE notifications, with
an RPC channel for querying the device and a NeoPixel showing battery level.
The design contract — requirements, wire format, GATT layout, opcodes, and the
board-support gaps — is [`README.md`](README.md).

**Nothing is implemented yet.** This area is currently `README.md` alone, and
that file is the contract: a behaviour change lands there *first*, then in the
firmware. The layout below describes files that do not exist yet and is a plan,
not an inventory.

This is a *freestanding* Zephyr application built against the tree in
`~/zephyrproject`, so `ZEPHYR_BASE` must be exported before `west build`:

```sh
export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -b adafruit_feather_nrf52840/nrf52840/sense/uf2 -p auto .
west flash            # copies zephyr.uf2 to the bootloader drive (FTHRSNSBOOT)
west build -t menuconfig
```

Enter the bootloader by tapping reset twice quickly. Console (Zephyr shell +
log) is a USB CDC endpoint the board variant sets up on its own, so the baud is
ignored — `pyserial-miniterm /dev/ttyACM0 115200`. Zephyr in this workspace is
4.4.99; the SDK is at `~/zephyr-sdk-1.0.1`.

Host tests run on `native_sim`, not on the target:

```sh
west build -b native_sim -p auto -d build_test tests/codec && ./build_test/zephyr/zephyr.exe
# or: west twister -p native_sim -T tests
```

## Must-knows

Full rationale and the measurements behind most of these are in
[`README.md`](README.md); this list is pointers, not the narrative. The items
marked *verified* were established by an actual `west build` of the overlay;
everything about how the hardware behaves is still unverified and README's
"known limitations and open questions" says which.

- **Zephyr has no driver for this board's IMU, and `st,lsm6dsl` is not the
  answer.** `drivers/sensor/st/` ships `lsm6ds0`, `lsm6dsl`, `lsm6dso`,
  `lsm6dso16is` and the `lsm6dsv*` family; the LSM6DS33 and LSM6DS3TR-C are in
  none of them. `st,lsm6dsl` is tempting — its `WHO_AM_I` check expects `0x6a`,
  which the DS3TR-C answers — but it rejects the DS33 (`0x69`) and has **no FIFO
  support**, so it cannot batch from a hardware-clocked source. The plan is an
  app-local driver over ST's own register driver, already vendored at
  `~/zephyrproject/modules/hal/st/sensor/stmemsc/lsm6ds3tr-c_STdC/driver/`.
  Nothing is copied into this repo.

- **`CONFIG_USE_STDC_LSM6DS3TR_C` cannot go in `prj.conf`** *(verified)*.
  `modules/hal_st/Kconfig:168` declares it as a bare `bool` with no prompt, so
  assigning it in a config file is a hard error that stops the build at Kconfig
  time. The out-of-tree driver's own Kconfig must `select` it, the way
  `drivers/sensor/st/lsm6dso/Kconfig:18` selects its own.

- **`CONFIG_CLOCK_CONTROL_NRF=y` is required, or the NeoPixel driver will not
  compile** *(verified)*. Left at its default (unset — this board pulls in the
  newer split HFCLK/LFCLK drivers), `drivers/led_strip/ws2812_gpio.c:139` takes
  its `#else` branch, which has bit-rotted: it calls
  `nrf_clock_control_release(drv_data->clk_dev, ...)` against a `drv_data` the
  surrounding function no longer declares. It is a build error, not a runtime
  one. Do not work around it by dropping the NeoPixel — the board's two plain
  LEDs are red and blue, so they cannot express three battery bands.

- **`&i2c0` must be re-declared as `nordic,nrf-twim` in `app.overlay`.**
  `adafruit_feather_nrf52840_common.dtsi:92` gives it `nordic,nrf-twi`, the
  legacy non-DMA peripheral. That one line is the whole of the "use DMA where
  possible" requirement on this board; check it took with
  `grep CONFIG_I2C_NRFX_TWIM build/zephyr/.config`.

- **The board DTS declares exactly one of the six sensors.** Only `sht3xd@44`.
  The IMU, LIS3MDL, APDS9960 and NeoPixel are invisible to devicetree until
  `app.overlay` declares them, and two Kconfig defaults bite once they are:
  `CONFIG_LIS3MDL_ODR` defaults to the string `"0.625"` (Hz) and
  `CONFIG_LIS3MDL_FS` to `4` (±4 gauss). Set both explicitly. The battery
  divider (`vbatt`) and the button (`button0`, `INPUT_KEY_0`, alias `sw0`) *are*
  in the board DTS and need no overlay at all.

- **Sample data goes on its own CDC ACM instance, never the console.** The
  `sense/uf2` variant already creates `board_cdc_acm_uart` for console, shell and
  log via `boards/common/usb/cdc_acm_serial.dtsi`; the overlay adds a *second*
  instance for binary data. This is the structural fix for the defect that cost
  the CircuitPython port a trustworthy error counter — do not "simplify" by
  putting samples back on the console.

- **The IMU stream carries raw `int16` register values; the scale-factor RPC
  carries the units.** Do not "helpfully" pre-scale it on the device. That
  design is what keeps the 208 Hz path a `memcpy` off the FIFO burst, what stops
  gyro at ±2000 dps overflowing `int16`, and what makes the `get scale` opcode
  load-bearing rather than decorative. Every *other* stream is device-converted
  fixed point, because at ≤20 Hz the in-tree drivers are worth more than a saved
  multiply. The asymmetry is deliberate and README explains the cost.

- **Gyro precedes accel in the IMU sample, and the two share one timestamp.**
  That is the order the chip's FIFO and its `OUTX_L_G`-onward block produce them.
  Reordering means touching every sample for nothing, and splitting the read
  means two samples ~1 ms apart pretending to be one instant — which any
  downstream fusion inherits.

- **The wire format gets exactly one host-side definition**, in
  `host/feather_protocol.py`, with every constant carrying a
  `# Must match ../src/<file>.cpp: <SYMBOL>` comment. The readers and the rerun
  viewer import it; they do not restate it.

- **Quote the device-timestamp rate, not the host arrival rate.** `dev` is
  `(count - 1) × 1000 / (last_ts - first_ts)` and needs no clock sync; `host` only
  says the link kept up. Divide by *measured* elapsed, never the nominal window —
  the CircuitPython port's tooling over-reported ~10 % that way and every number
  it published was wrong in the flattering direction.

- **Pure logic goes in Zephyr-header-free translation units.** `src/codec.cpp`
  and `src/battery_level.cpp` are compiled into both the firmware and the
  `native_sim` ztest under `tests/`. Pulling a Zephyr header into either breaks
  the host build. Note too that Zephyr builds C++ against its
  minimal libc++ with `-nostdinc++`: `<cstdint>` exists, `<cmath>`/`<cstdlib>` do
  not, so tests use the C headers.

- **The `i2c` shell wants the devicetree node's full name, not its label.**
  `i2c scan i2c@40003000`, not `i2c scan i2c0`. Tab completion is the reliable
  way to get it. `i2c read_byte i2c@40003000 0x6a 0x0f` is what settles which IMU
  this board actually carries (`0x69` = LSM6DS33, `0x6a` = LSM6DS3TR-C) — record
  the answer in README and delete that open question.

- **Do not restate an unverified hardware fact as known.** The NeoPixel's pin
  (`P0.16`), whether INT1/DRDY/INT are routed to GPIOs at all, and every
  throughput figure in README are inherited or inferred, not measured here. README
  keeps them separated from what was checked, and that separation is the document's
  main value — the CircuitPython port's README carries a correction recording what
  designing on an unmeasured claim cost it.

- **The host side is its own pixi environment** (`host/`), because neither bleak
  nor rerun is a repo-wide dependency. It carries
  `type = "pyrefly check -p default ."` on the command line rather than a
  `pyrefly.toml`. `rerun-sdk` comes from
  `[pypi-dependencies]`; the conda build is not what upstream ships, and the PyPI
  package named plain `rerun` is an unrelated file watcher.

- **This directory carries its own `.clang-format`, copied from Zephyr's.**
  Without it the repo-wide `fmt` would reflow these files away from Zephyr's
  tabs-and-100-columns style. Do not delete it.

- **`build/` (and `build_test/`) hold symlinks into `~/zephyrproject`** — a fmt
  glob that follows them, rather than using `git ls-files` as the root `fmt-c`
  task does, would edit the Zephyr installation in place. Both are gitignored.

- **This area builds with west/CMake**, not Bazel, pixi, or cargo — so
  `ZEPHYR_BASE` must be exported before `west build`.

## Layout

Only `README.md` and this file exist today. The rest is the plan it describes.

- `README.md` — the contract: requirements, hardware notes, design, wire format,
  GATT and RPC tables, and the open questions. **Changes land here first.**
- `CMakeLists.txt` — freestanding-app boilerplate, plus the `git describe`
  compile definition the `get build id` opcode reports.
- `prj.conf` — C++20, I²C/TWIM, the sensor drivers with their ODR and full-scale
  overrides, `CONFIG_CLOCK_CONTROL_NRF=y`, the BLE MTU tier, `CONFIG_HWINFO`, and
  the shell tier (`I2C_SHELL` earns its place here more than anywhere in the repo).
- `app.overlay` — the four missing devicetree nodes, the TWIM re-declaration, and
  the second CDC ACM instance. Reproduced verbatim in README.
- `drivers/` — the app-local LSM6DS33/LSM6DS3TR-C sensor driver over ST's vendored
  stmemsc code, with the Kconfig that `select`s `USE_STDC_LSM6DS3TR_C`.
- `src/main.cpp` — device readiness checks and thread startup, in dependency order.
- `src/imu.cpp` — FIFO watermark drain at 208 Hz into a batch.
- `src/magn.cpp`, `src/env.cpp`, `src/battery.cpp`, `src/buttons.cpp` — the
  lower-rate streams; buttons is an `input` subsystem callback with no thread.
- `src/codec.{hpp,cpp}` — batch header pack/unpack and COBS. Zephyr-header-free,
  shared with the host test.
- `src/battery_level.{hpp,cpp}` — divider mV → percent and the hysteresis band
  function. Zephyr-header-free, shared with the host test.
- `src/led.cpp` — the NeoPixel, repainted only on a band change.
- `src/ble.cpp` — the GATT service, per-rate-class characteristics, MTU-derived
  batch sizing, and the RPC pair.
- `src/usb.cpp` — COBS framing onto `cdc_acm_data`.
- `src/rpc.cpp` — the five opcodes, shared by both transports.
- `tests/codec/`, `tests/battery_level/` — `native_sim` ztest suites over the two
  pure modules.
- `host/feather_protocol.py` — the single host-side definition of the wire format,
  the decoders, and the RPC client. No I/O.
- `host/read_serial.py`, `host/read_ble.py` — the two rate CLIs.
- `host/feather_rerun.py` — the rerun viewer, either transport by flag.
- `host/pixi.toml` — the environment for all four (`pixi run serial`,
  `pixi run ble`, `pixi run viz`, `pixi run type`).
- `.clang-format` — Zephyr's, scoping this area's style away from the repo default.

`build/` is produced by west and is already covered by the repo `.gitignore`;
`build_test/` and `twister-out/` need a local `.gitignore`.
