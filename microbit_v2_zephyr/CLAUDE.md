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

Full rationale, measurements, and derivations for most of these live in the relevant
source file's comments and/or in [`README.md`](README.md); this list is pointers, not
the narrative.

- **`input report 1 30 1` starts an audio capture; use it instead of pressing button A.**
  `buttons.c`'s callback can't tell a synthetic event from a real one, so the whole path
  runs, BLE notification included — and it's the only acoustically clean trigger, since
  pressing A taps the PCB right next to the microphone. (Code 48 is button B.)

- **A module's `LOG_MODULE_REGISTER` level is a compile-time ceiling that
  `log_filter_set()` clamps the runtime level to.** All modules but `display` register at
  `LOG_LEVEL_DBG` so `log enable dbg <module>` works at all; `prj.conf`'s
  `CONFIG_SHELL_BACKEND_SERIAL_LOG_LEVEL_INF` is the other half — it's what keeps the
  console quiet at boot despite that ceiling. See README's "runtime log levels".

- **The `adc` shell command reconfigures the same SAADC channel the app captures on** —
  `read <ch>` is what applies it, the shell's own copy of the config starts zeroed so
  every field must be set, and after a retune `audio raw`'s counts stay true but its
  millivolts don't. See README's "three things that will surprise you".

- **`SAMPLE_RATE_HZ` (31311, not the 31250 that `1000000/32µs` suggests) must stay
  derived** as `16000000 / (SAMPLE_INTERVAL_US * 16 - 1)`, per Zephyr's `interval_to_cc()`
  (`drivers/adc/adc_nrfx_saadc.c:503`). Hardcoding either constant lets them drift apart
  with no build error. See `audio.c` and README's "known limitations".

- **`run_capture()` holds the HF crystal on for the capture, and that's load-bearing for
  accuracy** — HFCLK otherwise runs off the internal RC except when BLE borrows it per
  radio event, adding ~0.5 % error. `audio hfxo off` restores the old behaviour. See
  README's "known limitations".

- **Take HFCLK with `nrf_clock_control_request_sync()`, never `clock_control_on()`.** The
  two aren't interchangeable — a second context requesting a clock the other already holds
  latches the onoff manager in an unrecoverable error state with no notification. This
  once hung the temperature thread forever on the very first capture. If a stream stops
  silently while its neighbours keep running, read the thread's state over SWD
  (`pyocd commander -M attach`) before guessing. See README's "known limitations".

- **The mic channel is `ADC_GAIN_4` + `ADC_REF_INTERNAL` (full scale 150 mV), and
  `zephyr,vref-mv` must stay 600** — deliberately, to keep the signal out of the ADC's
  quantisation floor. Omitting `zephyr,vref-mv` silently converts everything to 0 mV
  instead of erroring (`adc_dt_spec` defaults it to 0). See `app.overlay` and README's
  "known limitations".

- **`sensor get lsm303agr-accel@19` answers `Read failed` while the app runs, and that's
  correct** — `accel.c` polls and consumes each data-ready, so the shell's own fetch gets
  `-ENODATA`. `sensor get temp@4000c000` has no such contention.

- **`CONFIG_SENSOR_SHELL` costs 14 KB flash / 6.6 KB RAM** (it pulls in RTIO via
  `SENSOR_ASYNC_API`) — drop it first if the build needs to shrink. `CONFIG_INPUT_SHELL`
  costs under a kilobyte.

- **This is the repo's only west/CMake area** — not Bazel, pixi, or cargo — and a
  *freestanding* Zephyr application outside `~/zephyrproject`, so `ZEPHYR_BASE` must be
  exported before `west build`.

- **There are two toolchains here**: west/CMake for `src/`, and a nested pixi environment
  in `host/` for the four host programs. `pixi run` in this area means `cd host` first —
  bleak, rerun-sdk, and pyserial live there, not in the root (dev-tools-only) environment.

- **The host programs come in two pairs on different ports** —
  `ble_stream.py`/`ble_rerun.py` over BLE, `tones.py`/`tone_sweep.py` over the console
  shell (`/dev/ttyACM0`). A sweep can't run while a `pyserial-miniterm` (or anything else)
  holds the port.

- **A tone sweep's `margin dB` column decides whether `error %` means anything** —
  `peak_frequency()` always returns *something*, so a silent rig yields a full table of
  confident nonsense. Under ~6 dB the peak is room rumble; check `pactl list sinks short`
  shows the sink RUNNING before trusting a sweep.

- **`ble_stream.py` is the single host-side definition of the wire format** (UUIDs,
  `struct` formats, decoders, `find_device`); `ble_rerun.py` imports rather than restates
  them. A new consumer should read a decoder's `value`, not its `text`.

- **rerun is PyPI's `rerun-sdk`, imported as `rerun`** — the plain `rerun` package on PyPI
  is an unrelated file watcher. `rerun-sdk` is this area's only `[pypi-dependencies]`
  entry, so its lock isn't all-conda; `~/code/remapy` sources it the same way.

- **Do not switch the button-state plot to a `StateTimelineView`: it cannot scroll with
  the time cursor in rerun 0.36.** It's the natural fit for press/release, but
  `re_view_state_timeline` doesn't implement `supports_visible_time_range`, and the
  failure is silent — `VisibleTimeRanges` is accepted and written into the blueprint
  exactly as for a plot, then ignored at render time. Verified on hardware; both classes
  are marked unstable, so re-test on a version bump. See README's "why button state is a
  plot and not a StateTimelineView".

- **`--window` reaches three of the four rerun views** — the button `TextLogView` is
  deliberately unwindowed so the full press history stays readable.

- **`ble_rerun.py`'s `--accel-batch-time` defaults to `spread`**, back-dating each batch's
  ten samples across the firmware's nominal 100 Hz instead of stamping them all at
  arrival time — otherwise the batching shows up as ten points stacked on one instant.
  Host clock only; no device-clock anchoring either way.

- **`client.mtu_size` is not the negotiated MTU on BlueZ** — it's a placeholder 23 until a
  writable characteristic is acquired, and all three here are notify-only. The firmware
  logs the real value on subscribe; `ble_stream.py` infers a lower bound from payload size
  instead.

- **The microphone is analog, captured via SAADC on AIN3 (P0.05) with enable on P0.20** —
  not the PDM/DMIC driver (`pdm0` is disabled on this board). Neither pin is in any Zephyr
  board file; both come from micro:bit hardware docs, declared in `app.overlay`.

- **The SAADC hardware sample timer needs three preconditions** (`start_read()` in
  `drivers/adc/adc_nrfx_saadc.c:552`): one active channel, `callback == NULL`,
  `interval_us` ≤ 128. Miss one and it silently falls back to software timing, too slow to
  hold this rate.

- **The accelerometer is polled, not interrupt-driven, because the board DTS omits a
  pull-up on P0.25 — not because of the pin's polarity**, which is already correct for the
  board's inverting transistor stage. Confirmed on hardware (P0.25 sits low forever after
  the first data-ready). Polling is kept regardless, since the line is shared with the
  magnetometer and the interface MCU. Full analysis and the overlay fix that would enable
  the trigger are in `accel.c` and README's "known limitations".

- **`CONFIG_LIS2MDL=n` is deliberate** — the magnetometer driver defaults on and would arm
  an interrupt on that same never-rising P0.25, for a sensor nothing here reads.

- **The peripheral must not call `bt_gatt_exchange_mtu()`** — against BlueZ the request
  draws no response and the connection drops after a 30 s timeout. Let the central
  negotiate; BlueZ settles on 247. `CONFIG_BT_GATT_CLIENT` is unset for the same reason:
  nothing here is a GATT client.

- **Peripheral ownership is already crowded**: `timer4`+`pwm0` (LED matrix), `pwm1`
  (buzzer), `timer0`/`rtc0`/RADIO (BLE controller). Anything new has to avoid all of them
  — the SAADC is safe, since it drives its own `SAMPLERATE` timer.

- **`CONFIG_CMSIS_DSP` and `CONFIG_MICROBIT_DISPLAY` have no `default y`** and must be set
  explicitly, unlike `TEMP_NRF5`/`LIS2DH`/`BUZZER_PWM`, which come up from devicetree on
  their own.

- **Use `arm_rfft_fast_init_2048_f32()`, not `arm_rfft_fast_init_f32()`.** The
  size-dispatching variant links every twiddle table from 32 to 4096 points into flash.

- **Button A must never buzz** — the speaker sits next to the microphone on the same PCB.
  The buzzer fires on B only, suppressed while `capture_active` is set.

- **Raising `CONFIG_BT_L2CAP_TX_MTU` past its 23-byte default is what makes accelerometer
  batching mean anything** — at the default, a notification carries three samples.
  `CONFIG_BT_BUF_ACL_TX_SIZE` has to go up with it.

- **The die temperature sensor is shared** with Zephyr's own LF clock calibration (this
  board's LF clock runs off the internal RC) — safe, since `temp_nrf5.c` mutex-guards each
  conversion, but the two do contend.

- **This directory carries its own `.clang-format`, copied from Zephyr's** — without it,
  the repo-wide `fmt` would reformat these files away from Zephyr's tabs-and-100-columns
  style. Do not delete it.

- **`build/` holds symlinks pointing outside the repo, into `~/zephyrproject`** — a glob
  that follows them (rather than using `git ls-files`, as the root `fmt-c` task does) will
  edit the Zephyr installation in place. See the root `CLAUDE.md`.

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
- `host/tones.py` — writes the test tones as 48 kHz WAVs, faded at both ends so the
  on/off transient does not smear the spectrum. Also the single definition of the
  frequency ladder and of `bin_centre_hz()`, which `tone_sweep.py` imports. Needs no
  hardware.
- `host/tone_sweep.py` — plays each tone, triggers a capture with `input report 1 30 1`
  over the console shell, parses `audio spectrum`, and tabulates the reported peak against
  the tone: error, strongest bin, margin over the noise floor, and capture wall time.
- `host/pixi.toml` — the environment for all four (`pixi run stream`, `pixi run viz`,
  `pixi run tones`, `pixi run sweep`, `pixi run type`).
- `.clang-format` — Zephyr's, scoping this area's style away from the repo default.

`build/` is produced by west and is already covered by the repo `.gitignore`.
