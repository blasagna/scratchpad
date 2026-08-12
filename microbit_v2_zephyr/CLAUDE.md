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

Console is `uart0` at 115200. Zephyr in this workspace is 4.4.99; the SDK is at
`~/zephyr-sdk-1.0.1`.

## Must-knows

- **This is the repo's only west/CMake area** — not Bazel, pixi, or cargo — and it is a
  *freestanding* Zephyr application living outside `~/zephyrproject`, so `ZEPHYR_BASE`
  must be exported before `west build`.

- **The microphone is analog and is captured via SAADC on AIN3 (P0.05), with enable on
  P0.20.** Do not reach for the PDM/DMIC driver: `pdm0` is `status = "disabled"` on this
  board and the mic is not wired to it. Neither pin appears anywhere in Zephyr's board
  files — both come from the micro:bit hardware docs, and `app.overlay` is where they are
  declared.

- **The SAADC hardware sample timer has three preconditions** (`start_read()` in
  `drivers/adc/adc_nrfx_saadc.c:552`): exactly one active channel,
  `options->callback == NULL`, and `interval_us` ≤ 128. Violate any one and the driver
  silently falls back to software-timed sampling, which cannot hold 31250 Hz. `interval_us`
  is an integer, so 32 µs → 31250 Hz exactly — that is why the sample rate is 31250 and
  not 32000.

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

## Layout

- `CMakeLists.txt` — the freestanding-application boilerplate;
  `find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})`.
- `prj.conf` — Kconfig for the app. The BLE MTU settings and the CMSIS-DSP component
  selection are the parts that are easy to get wrong.
- `app.overlay` — enables `&adc` (disabled in the SoC dtsi) with a single AIN3 channel,
  and declares the mic-enable GPIO under `zephyr,user`.
- `src/main.c` — device readiness checks and thread startup.
- `src/accel.c` — LSM303AGR at 100 Hz via the data-ready trigger, into a ring buffer.
- `src/temp.c` — 1 Hz poll of the nRF52833 die sensor.
- `src/buttons.c` — `input` subsystem callback: BLE events for both buttons, buzz on B,
  audio trigger on A.
- `src/audio.c` — SAADC block capture, Hann window, Welch-averaged 2048-point FFT,
  parabolic peak interpolation.
- `src/display.c` — `mb_display` wrapper for scrolling the frequency.
- `src/ble.c` — the GATT service, subscription state, and MTU-derived accel batching.
- `.clang-format` — Zephyr's, scoping this area's style away from the repo default.

`build/` is produced by west and is already covered by the repo `.gitignore`.
