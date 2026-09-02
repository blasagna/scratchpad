# feather_sense_zephyr

A Zephyr RTOS application for the Adafruit Feather Bluefruit Sense (nRF52840):
IMU, magnetometer, humidity/temperature, light, battery and button sampled on
one I²C bus and streamed to a host over both USB CDC and BLE notifications, with
an RPC channel for querying the device and a NeoPixel showing battery level.
The design contract — requirements, wire format, GATT layout, opcodes, the
board-support gaps, and what has and has not been verified on hardware — is
[`README.md`](README.md). **A behaviour change lands there first, then in the
firmware.**

This is a *freestanding* Zephyr application built against the tree in
`~/zephyrproject`, so `ZEPHYR_BASE` must be exported before `west build`:

```sh
export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -b adafruit_feather_nrf52840/nrf52840/sense/uf2 -p auto .
west flash            # copies zephyr.uf2 to the bootloader drive (FTHRSNSBOOT)
west build -t menuconfig
```

`west flash` needs the board in the UF2 bootloader. `fs bootloader` at the shell
gets it there without touching the board; a double-tap of reset is the fallback.
Zephyr in this workspace is 4.4.99; the SDK is at `~/zephyr-sdk-1.0.1`.

Host tests run on `native_sim`, not on the target:

```sh
west build -b native_sim -p auto -d build_test tests/codec && ./build_test/zephyr/zephyr.exe
west twister -p native_sim -T tests        # 52 cases, 4 configurations
```

## Must-knows

Full rationale and the measurements behind these are in [`README.md`](README.md);
this list is pointers, not the narrative. Items marked *measured* were
established on a real board.

- **This board carries an LSM6DS33** (`WHO_AM_I` = `0x69`), not the
  LSM6DS3TR-C *(measured)*. Zephyr has a driver for neither: `drivers/sensor/st/`
  ships `lsm6ds0`, `lsm6dsl`, `lsm6dso`, `lsm6dso16is` and the `lsm6dsv*` family.
  `st,lsm6dsl` is tempting and would reject this part outright — its `WHO_AM_I`
  check demands `0x6a` — and it has **no FIFO support**, so it could not batch
  from a hardware-clocked source either. `drivers/lsm6ds3trc/` is an app-local
  module over ST's vendored register driver at
  `~/zephyrproject/modules/hal/st/sensor/stmemsc/lsm6ds3tr-c_STdC/driver/`, and
  accepts both ids. Nothing is copied into this repo.

- **Three Kconfig symbols here have no prompt and cannot be assigned in
  `prj.conf`** *(measured — each one is a hard configuration error)*:
  `USE_STDC_LSM6DS3TR_C` (the out-of-tree driver's Kconfig must `select` it, the
  way `drivers/sensor/st/lsm6dso/Kconfig:18` selects its own), `I2C_NRFX_TWIM`
  (set by the overlay's `nordic,nrf-twim` and by nothing else — verify with
  `grep CONFIG_I2C_NRFX_TWIM build/zephyr/.config`), and `HAS_STMEMSC`.

- **`CONFIG_USE_DT_CODE_PARTITION=y` prevents bricking the bootloader**
  *(measured)*. Neither `*_uf2_defconfig` sets it, so the image links at flash
  offset 0 and the UF2 reports `start address: 0x0`. Flashing that erases the
  s140 SoftDevice **and the MBR at 0x0 that the Adafruit bootloader needs**,
  leaving SWD — pads only on this board — as the way back. The ItsyBitsy
  nRF52840 has the same bootloader and layout and does set it; this is an
  upstream gap. Check the `west build` output line
  `Converted to uf2, ... start address: 0x26000` — **not** `.config`, where
  `CONFIG_FLASH_LOAD_OFFSET` appears as `=0` when the setting is missing and
  vanishes entirely when it is present, so grepping it reads backwards.

- **`CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n`, and `src/usb.cpp` does the
  usbd setup** *(measured)*. Zephyr's boot-time initializer registers only the
  first CDC ACM instance — its own comment says so — so with it in charge the
  data port is built, bound to a Zephyr device, and never enumerated: writes
  succeed and go nowhere. `usbd_register_all_classes()` takes both.

- **Sample data goes on its own CDC ACM instance, never the console**, and the
  two are told apart by their **USB interface string descriptors**, not by
  enumeration order. The `label` property on a `zephyr,cdc-acm-uart` node
  *becomes* that descriptor; `host/read_serial.py`'s `find_port()` matches
  `"Feather Sense data"` against `/sys/class/tty/ttyACM*/device/interface` and
  refuses to guess. Do not "simplify" by putting samples on the console — that
  is the defect that cost the CircuitPython port a trustworthy error counter.

- **`CONFIG_CLOCK_CONTROL_NRF=y` is required, or the NeoPixel driver will not
  compile** *(measured)*. Left at its default,
  `drivers/led_strip/ws2812_gpio.c:139` takes an `#else` branch that has
  bit-rotted — it calls `nrf_clock_control_release(drv_data->clk_dev, ...)`
  against a `drv_data` the surrounding function no longer declares. The symbol
  is deprecated in 4.4.99 and warns; do not work around it by dropping the
  NeoPixel, since the board's two plain LEDs are red and blue and cannot express
  three battery bands.

- **The battery divider *does* need an overlay** *(measured)*, contrary to what
  the design document originally said. The board DTS declares the `vbatt`
  `voltage-divider` node but no `channel@5` under `&adc`, and without one every
  reading is **0 mV with no error** — `adc_channel_setup_dt()` configures
  nothing, and `adc_raw_to_millivolts_dt()` multiplies by a `DT_PROP_OR(..., 0)`
  reference. Reach the node with `DT_PATH(vbatt)`; it has no label.

- **The SHT30 runs in periodic mode, not single-shot** *(measured)*. That voids
  the design's largest performance worry (the CircuitPython port's ~152 ms/s was
  never applicable) and introduces a different one: the chip NACKs `FETCH_DATA`
  when no measurement is ready, so a 1 Hz reader against a 1 MPS chip
  intermittently fails. `CONFIG_SHT3XD_MPS_2=y` is the fix. `fs env` reports the
  fetch cost in microseconds and counts failures.

- **The board DTS declares exactly one of the six sensors**, `sht3xd@44`. The
  IMU, LIS3MDL, APDS9960 and NeoPixel are invisible to devicetree until
  `app.overlay` declares them, and two Kconfig defaults bite once they are:
  `CONFIG_LIS3MDL_ODR` defaults to the string `"0.625"` (Hz) and
  `CONFIG_LIS3MDL_FS` to `4` (±4 gauss). Set both explicitly. The button
  (`button0`, `INPUT_KEY_0`, alias `sw0`) *is* in the board DTS and needs
  nothing.

- **The IMU stream carries raw `int16` register values; the scale-factor RPC
  carries the units.** Do not "helpfully" pre-scale it on the device. That is
  what keeps the 208 Hz path a `memcpy` off the FIFO burst, what stops gyro at
  ±2000 dps overflowing `int16`, and what makes `get scale` load-bearing rather
  than decorative. Every *other* stream is device-converted fixed point.

- **The scale table is in *nano*-SI, so a dimensionless field is `1e9/1`, not
  `1/1`** *(measured, the hard way)*. Getting it wrong is silent and looks like a
  dead sensor: the light channel reported a good count of 129 and the host
  printed `0.0000`. `kRawCount` in `src/rpc.cpp` exists so it can only be got
  wrong once. Relatedly, `SENSOR_CHAN_LIGHT` on the APDS9960 is the **raw clear
  channel count**, not lux (`drivers/sensor/apds9960/apds9960.c:275`).

- **The magnetometer's wire unit is deci-µT** *(measured)*. Centi-µT — which the
  design originally specified — puts the LIS3MDL's own ±400 µT full scale at
  ±40000, past an `int16`, so the *wire* clips before the sensor does. The first
  board sat in a ~570 µT field and railed all three axes at once. Deci-µT costs
  nothing real: the 0.1 µT step is below the part's ~0.32 µT RMS noise.

- **Gyro precedes accel in the IMU sample, and the two share one timestamp.**
  That is the order the chip's FIFO and its `OUTX_L_G`-onward block produce them.
  Reordering means touching every sample for nothing, and splitting the read
  means two samples ~1 ms apart pretending to be one instant — which any
  downstream fusion inherits.

- **The serial link carries a channel byte inside the COBS frame**:
  `cobs([channel] + payload) + 0x00`, `0x01` for a batch and `0x02` for RPC.
  Batches and RPC replies share the one pipe device-to-host, so direction alone
  cannot tell them apart. It is the serial analogue of a GATT characteristic; the
  `payload` bytes stay identical on both transports.

- **The wire format gets exactly one host-side definition**, in
  `host/feather_protocol.py`, with every constant carrying a
  `# Must match ../src/<file>: <SYMBOL>` comment. The readers own their
  transports; `feather_rerun.py` imports `SerialLink`/`BleLink` from them.

- **Quote the device-timestamp rate, not the host arrival rate.** `dev` is
  `(count - 1) × 1000 / (last_ts - first_ts)` and needs no clock sync; `host`
  only says the link kept up. Divide by *measured* elapsed, never the nominal
  window — the CircuitPython port's tooling over-reported ~10 % that way. A
  stream with one sample in a window correctly reports `dev 0.00`: one sample
  spans no intervals.

- **Pure logic goes in Zephyr-header-free translation units.** `src/codec.cpp`
  and `src/battery_level.cpp` are compiled into both the firmware and the
  `native_sim` ztests under `tests/`. Pulling a Zephyr header into either breaks
  the host build. Zephyr also builds C++ against its minimal libc++ with
  `-nostdinc++`: `<cstdint>` exists, `<cmath>`/`<cstdlib>`/`<initializer_list>`
  do not, so tests use the C headers and plain arrays.

- **The `i2c` shell wants the devicetree node's full name, not its label.**
  `i2c scan i2c@40003000`, not `i2c scan i2c0`. Tab completion is the reliable
  way to get it.

- **BLE is built but has never run.** No code path from `bt_gatt_notify` onward
  has executed — the only host available had its Bluetooth adapter
  rfkill-blocked. Do not describe any BLE behaviour here as working, and do not
  quote a BLE throughput number as this firmware's; the ones in README are the
  CircuitPython port's, as a floor.

- **Do not restate an unverified hardware fact as known.** README keeps three
  lists — settled by running it, live limitations, still unverified — and that
  separation is the document's main value. The NeoPixel's pin (`P0.16`), whether
  INT1/DRDY/INT are routed, and every BLE figure are inherited or inferred, not
  measured here.

- **The host side is its own pixi environment** (`host/`), because neither bleak
  nor rerun is a repo-wide dependency. It carries
  `type = "pyrefly check -p default ."` on the command line rather than a
  `pyrefly.toml`. `rerun-sdk` comes from `[pypi-dependencies]`; the conda build
  is not what upstream ships, and the PyPI package named plain `rerun` is an
  unrelated file watcher.

- **This directory carries its own `.clang-format`, copied from Zephyr's.**
  Without it the repo-wide `fmt` would reflow these files away from Zephyr's
  tabs-and-100-columns style. Do not delete it.

- **`build/`, `build_test/` and `twister-out/` hold symlinks into
  `~/zephyrproject`** — a fmt glob that followed them, rather than using
  `git ls-files` as the root `fmt-c` task does, would edit the Zephyr
  installation in place. All three are gitignored.

- **This area builds with west/CMake**, not Bazel, pixi, or cargo.

## Layout

- `README.md` — the contract: requirements, hardware notes, design, wire format,
  GATT and RPC tables, measurements, and the three lists of what is and is not
  verified. **Changes land here first.**
- `CMakeLists.txt` — freestanding-app boilerplate, the `ZEPHYR_EXTRA_MODULES`
  entry for the out-of-tree driver (which must precede `find_package(Zephyr)`),
  and the `git describe` compile definition the `get build id` opcode reports.
- `VERSION` — the app version `get build id` reports alongside that.
- `prj.conf` — C++20, I²C/TWIM, the sensor drivers with their ODR and full-scale
  overrides, the code-partition and CDC-ACM lines above, the BLE MTU tier,
  `CONFIG_HWINFO`, `CONFIG_REBOOT`, and the shell tier.
- `app.overlay` — the four missing devicetree nodes, the battery ADC channel, the
  TWIM re-declaration, and the second CDC ACM instance with both `label`s.
  Reproduced verbatim in README.
- `drivers/lsm6ds3trc/` — the app-local sensor driver as a Zephyr module
  (`zephyr/module.yml`, its own `Kconfig`, and `dts/bindings/` including a
  `vendor-prefixes.txt` for the `scratchpad` prefix). `lsm6ds3trc.h` is the
  app-facing FIFO API; `lsm6ds3trc_trigger.c` is compiled only when the node has
  `irq-gpios`, which it currently does not.
- `src/main.cpp` — device readiness checks and thread startup, in dependency
  order. USB first: the console rides a CDC endpoint nothing brings up until it
  runs.
- `src/imu.cpp` — FIFO drain at 208 Hz into batches, with the stall clamp.
- `src/magn.cpp`, `src/env.cpp`, `src/battery.cpp`, `src/buttons.cpp` — the
  lower-rate streams; buttons is an `input` callback with no thread.
- `src/codec.{hpp,cpp}` — batch header, scale-field layout, and COBS.
  Zephyr-header-free, shared with the host tests.
- `src/battery_level.{hpp,cpp}` — divider mV → percent and the hysteresis band
  function. Zephyr-header-free, shared with the host tests.
- `src/streams.{hpp,cpp}` — the fan-out: sequence numbers, header packing, and
  the two transmit queues and their threads.
- `src/led.cpp` — the NeoPixel, repainted only on a band change.
- `src/ble.cpp` — the GATT service, per-rate-class characteristics, MTU-derived
  batch sizing, and the RPC pair.
- `src/usb.cpp` — the usbd setup for both CDC ACM instances, COBS framing onto
  `cdc_acm_data`, and the rx thread.
- `src/rpc.cpp` — the five opcodes and the scale table, shared by both transports.
- `src/shell.cpp` — the `fs` command group, including `fs bootloader`.
- `tests/codec/`, `tests/battery_level/` — `native_sim` ztest suites over the two
  pure modules.
- `host/feather_protocol.py` — the single host-side definition of the wire format,
  the decoders, the scale table, the RPC frames, and the rate arithmetic. No I/O.
- `host/read_serial.py`, `host/read_ble.py` — the transports and the two rate CLIs.
- `host/feather_rerun.py` — the rerun viewer, either transport by flag.
- `host/pixi.toml` — the environment for all four (`pixi run serial`,
  `pixi run ble`, `pixi run viz`, `pixi run type`).
- `.clang-format` — Zephyr's, scoping this area's style away from the repo default.
