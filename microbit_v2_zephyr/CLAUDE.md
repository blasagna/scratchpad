# microbit_v2_zephyr

A Zephyr RTOS application for the BBC micro:bit V2: accelerometer and MCU temperature
streamed over BLE notifications, button events with a buzzer, and a button-triggered
audio capture whose peak frequency is found with an on-device FFT and scrolled across the
LED matrix. Requirements, design, and the hardware gotchas are in
[`README.md`](README.md).

```sh
export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -b bbc_microbit_v2 -p auto .
west flash -r pyocd
west build -t menuconfig
```

Console is `uart0` at 115200, and it carries a Zephyr shell as well as the log —
`pyserial-miniterm /dev/ttyACM0 115200`. Zephyr in this workspace is 4.4.99; the SDK is at
`~/zephyr-sdk-1.0.1`.

## Must-knows

- **`input report 1 30 1` starts an audio capture; use it instead of pressing button A.**
  `buttons.c` registers `INPUT_CALLBACK_DEFINE(NULL, ...)`, so it cannot tell a synthetic
  event from a real one and the whole path runs, BLE notification included. Pressing A
  physically taps the PCB centimetres from the microphone and puts the thump in the block
  being measured, so this is the only acoustically clean trigger. (Code 48 is button B.)

- **A module's `LOG_MODULE_REGISTER` level is a compile-time ceiling, and
  `log_filter_set()` clamps the runtime level to it.** All six modules here register at
  `LOG_LEVEL_DBG` for exactly this reason: at `LOG_LEVEL_INF` the shell's
  `log enable dbg accel` answers "level set to inf" and silently does nothing. Raising the
  ceiling alone makes the console noisy, because the serial backend's initial limit
  defaults to "the system limit" and follows it — `CONFIG_SHELL_BACKEND_SERIAL_LOG_LEVEL_INF`
  is what holds the *initial runtime* level at inf. Both halves are needed; neither works
  alone.

- **The `adc` shell command reconfigures the same SAADC channel the app captures on.**
  There is one channel 3, set up once by `adc_channel_setup_dt()`, so a retune from the
  shell changes what the next capture records — that is how the gain measurement in
  `README.md` was made without a rebuild. Two traps: it is `adc … read <ch>` that calls
  `adc_channel_setup()` and applies the settings, and the shell's own copy of the config
  starts as all zeros, so every field must be set or `read` fails with
  `ADC resolution value 0 is not valid`. `audio raw`'s counts stay true across such a
  change but its millivolts do not — `adc_raw_to_millivolts_dt()` reads the compiled-in
  `app.overlay` values, not the live configuration.

- **`SAMPLE_RATE_HZ` is 31311, derived, and must stay derived.** It is
  `16000000 / (SAMPLE_INTERVAL_US * 16 - 1)`, because Zephyr's `interval_to_cc()`
  (`drivers/adc/adc_nrfx_saadc.c:503`) programs `SAMPLERATE.CC = interval_us * 16 - 1` and
  the nRF52833 samples at 16 MHz / CC. The obvious `1000000 / SAMPLE_INTERVAL_US` = 31250
  is wrong by 0.2 %, systematically, on every frequency reported. The rate and the interval
  are one knob — hardcoding either lets them drift apart with no build error.

- **`run_capture()` holds the HF crystal on, and that is load-bearing for accuracy.**
  HFCLK otherwise runs from the internal RC except when the BLE controller borrows the
  crystal per radio event, which put another ~0.5 % of error on the reported frequency.
  `audio hfxo off` restores the old behaviour for re-measuring; default is on. Together
  with the derived rate this took 3 kHz from −0.68 % to −0.005 %. See `README.md`,
  "known limitations", for the full measurement and the one inference in it that was not
  read off a datasheet.

- **Take HFCLK with `nrf_clock_control_request_sync()`, never `clock_control_on()`.** Both
  work on the same `hfclk` device and look interchangeable. They are not:
  `clock_control_nrf_common.c` tags the clock with the starting context and refuses a
  second one with `-EPERM`, and a failed onoff start transition latches the manager in an
  error state that the non-54H request path never resets. The nRF die-temperature driver
  requests HFCLK for every conversion, so `clock_control_on()` in `run_capture()` hung the
  temperature thread on `K_FOREVER` at the first capture and the BLE temperature stream
  went silent until reset — with nothing in the log. If a stream stops while its neighbours
  keep running, read the thread's `thread_state`/`pended_on` over SWD before guessing;
  `pyocd commander -M attach` works without disturbing the console. Full write-up in
  `README.md`, "known limitations".

- **The mic channel is `ADC_GAIN_4` + `ADC_REF_INTERNAL`, full scale 150 mV, and that is
  deliberate.** The obvious `ADC_GAIN_1_4`/`ADC_REF_VDD_1_4` (full scale VDD) left the
  signal in the bottom 2 % of the range, where the FFT was mostly measuring the ADC's own
  quantisation: 4.0 dB of peak-to-noise-floor against a 1 kHz tone versus 18.6 dB now. The
  bias idles near 1746 counts, 43 % of range, ~29 dB below clipping. Two consequences:
  `zephyr,vref-mv` must stay at 600 (the reference, not full scale), and a quiet capture
  now shows real room noise rather than a flat floor — that is the channel working, not a
  fault. See `README.md`, "known limitations".

- **`zephyr,vref-mv` is the reference voltage, not full scale, and omitting it converts
  everything to 0 mV without an error.** `adc_dt_spec` fills the field with
  `DT_PROP_OR(node, zephyr_vref_mv, 0)`. With `ADC_REF_VDD_1_4` the value is 825, and the
  `ADC_GAIN_1_4` inversion then puts full scale back at VDD.

- **`sensor get lsm303agr-accel@19` answers `Read failed` while the app runs, and that is
  correct.** `accel.c` polls at 100 Hz and consumes each data-ready, so the shell's fetch
  finds the bit clear and gets `-ENODATA` — the case `sample_once()` treats as normal.
  `sensor get temp@4000c000` has no such contention and works.

- **`CONFIG_SENSOR_SHELL` costs 14 KB of flash and 6.6 KB of RAM** — it `select`s
  `SENSOR_ASYNC_API`, which pulls in RTIO, and it is more RAM than the entire rest of the
  shell tier. Drop it first if the build needs to shrink. `CONFIG_INPUT_SHELL` is under a
  kilobyte.

- **This is the repo's only west/CMake area** — not Bazel, pixi, or cargo — and it is a
  *freestanding* Zephyr application living outside `~/zephyrproject`, so `ZEPHYR_BASE`
  must be exported before `west build`.

- **There are two toolchains here**: west/CMake for the firmware in `src/`, and a nested
  pixi environment in `host/` for the two host programs. They share nothing, so `pixi run`
  in this area means `cd host` first. bleak and rerun-sdk live in that environment rather
  than the root one, which is dev-tools-only.

- **`ble_stream.py` is the single host-side definition of the wire format.**
  `ble_rerun.py` imports the UUIDs, the `struct.Struct` formats, the decoders, and
  `find_device` from it — do not restate any of them in a second file. The decoders
  return both a preformatted `text` (what `ble_stream.py` prints) and a structured
  `value` (`AccelBatch` / `TempReading` / `ButtonEvent`, what `ble_rerun.py` plots);
  a new consumer should read `value` and leave `text` alone.

- **rerun comes from PyPI, not conda-forge, and the package is `rerun-sdk` — it imports
  as `rerun`.** The PyPI package named plain `rerun` is an unrelated file watcher; do not
  add it. `rerun-sdk` is the only `[pypi-dependencies]` entry here, so this is the one
  area whose lock is not all-conda; `~/code/remapy` sources it the same way. The
  conda-forge build of the same version exists but is not what upstream ships — take the
  wheel.

- **Do not switch the button-state plot to a `StateTimelineView`: it cannot scroll with
  the time cursor in rerun 0.36.** It is the natural fit — purpose-built for
  press/release, drawing named bands rather than a 0/1 line — and it was tried here and
  reverted. Windowing is a per-view-class opt-in in the viewer
  (`ViewClass::supports_visible_time_range`) and `re_view_state_timeline` does not
  implement it. **The failure is silent**: `VisibleTimeRanges` is accepted, stored, and
  written into the blueprint exactly as for a plot — verified in the saved `.rrd` — and
  then ignored at render time, so confirming that the property lands proves nothing.
  Verified on hardware with real button presses. `StateTimelineView` also has no
  `time_ranges` keyword (it can be forced via `view.properties["VisibleTimeRanges"]`,
  which is all the keyword does underneath, but that changes nothing here). Both it and
  `StateChange` are marked unstable, so re-test on a version bump — if windowing ever
  lands, the bands are the better view.

- **`--window` reaches three of the four views.** The `TextLogView` for button events is
  deliberately unwindowed so the press history stays readable; a log list scrolls on its
  own anyway.

- **Accelerometer samples arrive ten to a notification, so arrival-time stamping piles
  ten samples on one instant.** `ble_rerun.py` stamps on the host's monotonic clock only
  — the device's `t_ms` is deliberately not used as a timeline, so there is no clock
  anchoring to drift. `--accel-batch-time spread` back-dates within the batch at the
  firmware's nominal 100 Hz when a continuous waveform is wanted; it is off by default
  because the default is the literal truth about when the host learned each value.

- **`client.mtu_size` is not the negotiated MTU on BlueZ.** bleak's BlueZ backend returns
  a placeholder 23 (with a `UserWarning`) until the characteristic is acquired, and
  acquisition needs a writable characteristic — all three here are notify-only. Do not
  print it as if it were real: the firmware logs the true value on subscribe, and
  `host/ble_stream.py` infers a lower bound from the observed payload size instead.

- **The microphone is analog and is captured via SAADC on AIN3 (P0.05), with enable on
  P0.20.** Do not reach for the PDM/DMIC driver: `pdm0` is `status = "disabled"` on this
  board and the mic is not wired to it. Neither pin appears anywhere in Zephyr's board
  files — both come from the micro:bit hardware docs, and `app.overlay` is where they are
  declared.

- **The SAADC hardware sample timer has three preconditions** (`start_read()` in
  `drivers/adc/adc_nrfx_saadc.c:552`): exactly one active channel,
  `options->callback == NULL`, and `interval_us` ≤ 128. Violate any one and the driver
  silently falls back to software-timed sampling, which cannot hold this rate. `interval_us`
  is an integer, so 32 µs is the closest step to 32 kHz — that is why the sample rate is
  nominally 31 kHz and not 32000. The exact figure is 31311, not 31250; see the
  `SAMPLE_RATE_HZ` note above.

- **The accelerometer is polled, and the blocker is a missing pull-up — not the pin's
  polarity.** P0.25 is `COMBINED_SENSOR_INT`. On the V2 schematic each sensor's interrupt
  drives the base of its own DTC143E digital NPN (`T7` for the accelerometer's `INT1_XL`,
  `T5` for the magnetometer's DRDY) whose collector is that net; the interface MCU shares
  it too. The common-emitter stage inverts, so the chip's default active-high INT1 is the
  *correct* setting here and its polarity bit must be left alone — the board supplies both
  the inversion and the open-collector behaviour. Nothing pulls the net high, though, and
  `bbc_microbit_v2.dts` declares `irq-gpios = <&gpio0 25 GPIO_ACTIVE_HIGH>` with no
  `GPIO_PULL_UP`, so `lis2dh`'s bare `gpio_pin_configure_dt(..., GPIO_INPUT)` leaves the
  pin floating: the first data-ready drags it to 0 V and it stays there forever. Confirmed
  on hardware — P0.25 reads low indefinitely, zero samples arrive, and both
  `sensor_attr_set` and `sensor_trigger_set` still return success. Note `int1-gpio-config`
  defaults to `EDGE_BOTH`, which is polarity-agnostic, so polarity could not have been
  what blocked it.

  An overlay adding `(GPIO_ACTIVE_LOW | GPIO_PULL_UP)` plus
  `int1-gpio-config = <LIS2DH_DT_GPIO_INT_LEVEL_LOW>` on `&lsm303agr_accel` would make the
  trigger work. Keep the polling regardless — the line is shared, so the interface MCU can
  assert it while the accelerometer is idle and hold it low across the sensor's own
  assertions. If you do revisit this, change the overlay, never the chip's polarity bit.

- **`CONFIG_LIS2MDL=n` is deliberate.** The magnetometer driver is `default y` off the
  board DTS, and its trigger mode defaults to `GLOBAL_THREAD`, whose init unconditionally
  arms a rising-edge interrupt on that same never-rising P0.25 — for a sensor nothing here
  reads. Leaving it enabled costs flash, RAM, and sole ownership of the pin.

- **The peripheral must not call `bt_gatt_exchange_mtu()`.** Against BlueZ the request gets
  no response; the ATT transaction times out after 30 s and drops the connection
  (`MTU exchange failed (0x0e)`, then disconnect reason `0x16`). Let the central negotiate
  — BlueZ settles on 247, which is what `CONFIG_BT_L2CAP_TX_MTU` asks for. This is also why
  `CONFIG_BT_GATT_CLIENT` is *not* set: nothing here is a GATT client.

- **Peripheral ownership is already crowded.** `timer4` + `pwm0` belong to the LED matrix
  driver, `pwm1` to the buzzer, and `timer0`/`rtc0`/RADIO to the BLE controller. Anything
  new has to avoid all of them. The SAADC is safe — it drives its own `SAMPLERATE` timer
  and needs no TIMER instance.

- **`CONFIG_CMSIS_DSP` and `CONFIG_MICROBIT_DISPLAY` have no `default y`** and must be set
  explicitly. By contrast `TEMP_NRF5`, `LIS2DH`, and `BUZZER_PWM` all come up on their own
  from devicetree, so `CONFIG_SENSOR=y` and `CONFIG_BUZZER=y` are enough for those.

- **Use `arm_rfft_fast_init_2048_f32()`, not `arm_rfft_fast_init_f32()`.** The
  size-dispatching variant links every twiddle table from 32 to 4096 points into flash.

- **Button A must never buzz.** The speaker sits centimetres from the microphone on the
  same PCB, so a buzz on A would be measured by A's own FFT. The buzzer fires on B only,
  and is additionally suppressed while `capture_active` is set.

- **Raising `CONFIG_BT_L2CAP_TX_MTU` above its 23-byte default is what makes the
  accelerometer batching requirement mean anything.** At the default, a notification
  carries 20 bytes — three samples. `CONFIG_BT_BUF_ACL_TX_SIZE` (default 27) has to go up
  with it.

- **The die temperature sensor is shared.** The board runs its LF clock from the internal
  RC oscillator, so Zephyr's clock calibration samples `TEMP` too. This is safe —
  `temp_nrf5.c` guards each conversion with a mutex — but the two users do contend.

- **This directory carries its own `.clang-format`, copied from Zephyr's.** The repo-wide
  `pixi run fmt` globs every `*.c`/`*.h`, and without a local config it would reformat
  these files away from Zephyr's tabs-and-100-columns style. clang-format stops at the
  first `.clang-format` it finds walking up from a file, so this scopes Zephyr style to
  this area and leaves the rest of the repo alone. Do not delete it.

- **`build/` contains symlinks that point out of the repo, into `~/zephyrproject`.** Zephyr
  generates `build/zephyr/misc/generated/syscalls_links/include_zephyr_*` as symlinked
  *directories* to the real Zephyr headers. Any repo-wide tool that walks `**` and follows
  symlinks will edit the Zephyr installation in place — the root `fmt-c` task once did
  exactly that and reformatted 1268 headers, which broke the assembler macros and stopped
  the tree building. `fmt-c` now takes its file list from `git ls-files`, so ignored
  directories are skipped; keep it that way rather than reintroducing a bare glob.

## Layout

- `CMakeLists.txt` — the freestanding-application boilerplate;
  `find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})`.
- `prj.conf` — Kconfig for the app. The BLE MTU settings, the CMSIS-DSP component
  selection, and the two-part log-level arrangement above are the parts that are easy to
  get wrong.
- `app.overlay` — enables `&adc` (disabled in the SoC dtsi) with a single AIN3 channel,
  and declares the mic-enable GPIO under `zephyr,user`.
- `src/main.c` — device readiness checks and thread startup.
- `src/accel.c` — LSM303AGR at 100 Hz, polled on a `k_timer`, into a ring buffer.
- `src/temp.c` — 1 Hz poll of the nRF52833 die sensor.
- `src/buttons.c` — `input` subsystem callback: BLE events for both buttons, buzz on B,
  audio trigger on A.
- `src/audio.c` — SAADC block capture, Hann window, Welch-averaged 2048-point FFT,
  parabolic peak interpolation. Also the `audio raw` / `audio spectrum` shell commands,
  behind `#ifdef CONFIG_SHELL` at the end of the file: they read `raw[]` and `mag_avg[]`
  directly, which is why they live here rather than in a file of their own.
- `src/display.c` — `mb_display` wrapper for scrolling the frequency.
- `src/ble.c` — the GATT service, subscription state, and MTU-derived accel batching.
- `host/ble_stream.py` — host-side bleak reader: subscribes to all three characteristics,
  decodes them, prints button events, and measures throughput. Also the home of the wire
  format, the decoders, and `find_device`, which `ble_rerun.py` imports.
- `host/ble_rerun.py` — the same streams plotted live in rerun: accelerometer in m/s²,
  temperature in °F, and button state as a `StepAfter` staircase, all three as
  rolling-window plots, plus a `TextLogView` listing each button notification as
  received. The three plots are windowed; the event log is not.
- `host/pixi.toml` — the environment for both (`pixi run stream`, `pixi run viz`,
  `pixi run type`).
- `.clang-format` — Zephyr's, scoping this area's style away from the repo default.

`build/` is produced by west and is already covered by the repo `.gitignore`.
