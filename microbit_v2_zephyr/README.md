# bbc micro:bit v2 zephyr rtos application

Learn [Zephyr RTOS](https://docs.zephyrproject.org/latest/index.html) by building an application for the [micro:bit V2](https://docs.zephyrproject.org/latest/boards/bbc/microbit_v2/doc/index.html) board, which has several on board sensors, buttons, LEDs, and BLE communication capabilities.

This application sits in a repo outside of the standard zephyr workspace. See [zephyr application development](https://docs.zephyrproject.org/latest/develop/application/index.html#application), focusing on the "Zephyr freestanding application" pattern. The zephyr workspace is at ~/zephyrproject.

All six requirements have been run and checked on a real micro:bit V2. The measurements
behind that, and the three defects the hardware turned up, are in
[known limitations](#known-limitations) — most usefully, the accelerometer's data-ready
trigger cannot work on this board, so it is polled.

## requirements

1. Sample the accelerometer at 100 Hz continuously
1. Sample the MCU temperature continuously at 1 Hz
1. Capture press and release events for both the A and B buttons
1. Play a short audible buzz when button B is pressed. Button A stays silent, so it does
   not pollute its own audio capture.
1. When button A is pressed, capture audio for 1 second. Calculate the peak frequency of
   the audio using an on-device FFT computation. Expect audible tones from roughly 30 Hz
   to 10 kHz — the ceiling is the MEMS element's roll-off, not Nyquist. Print the
   frequency to the LED matrix display.
1. Make the device a BLE peripheral.
  1. Expose a characteristic to stream the accelerometer data in fixed-point values over
     notifications for high throughput. Batch the samples to maximize throughput over
     each connection interval.
  1. Expose a characteristic to stream the temperature data in notifications.
  1. Expose a characteristic to stream the A and B button press and release events in
     notifications.

## hardware notes

The gap between what the board *has* and what Zephyr's board support *enables* is the
main thing to understand before reading the code. Verified against Zephyr 4.4.99.

| Feature | Hardware | Zephyr board support | What this app does |
|---|---|---|---|
| Accelerometer | LSM303AGR on I²C @0x19, INT1 → P0.25 | `lsm303agr_accel`, `compatible = "st,lis2dh"`, enabled | Use the `lis2dh` driver's `SENSOR_TRIG_DATA_READY` |
| Buttons | A = P0.14, B = P0.23, active low | `gpio_keys` node with `INPUT_KEY_A` / `INPUT_KEY_B` | Use the `input` subsystem |
| LED matrix | 5×5, multiplexed | `nordic,nrf-led-matrix` + `mb_display` | `mb_display_print()` in scroll mode |
| Buzzer | Speaker on P0.00 | `buzzer` node, `compatible = "pwm-buzzer"`, on `&pwm1` | `buzzer_tone()` |
| Temperature | nRF52833 **die** sensor | `temp` node, already enabled | `SENSOR_CHAN_DIE_TEMP` |
| Microphone | **Analog** MEMS, out → **P0.05 (AIN3)**, enable → **P0.20** | **nothing — no node at all** | App supplies `app.overlay` enabling `&adc` |

Two consequences worth stating outright:

**The microphone is analog, not PDM.** The nRF52833 has a PDM peripheral and Zephyr has a
driver for it, but on this board `pdm0` is `status = "disabled"` and the microphone is not
wired to it. Audio capture goes through the **SAADC**. That turns out to be the better
path anyway: nRF52833 PDM tops out at 20.8 kHz (only `Ratio64`/`Ratio80` exist, max
PDM_CLK 1.333 MHz), while the SAADC does 200 ksps.

**The temperature is the CPU die, not the room.** The micro:bit V2 has no ambient
temperature sensor. The nRF52833's on-die sensor is the same source MakeCode's
`temperature` block reports, and it reads several degrees above ambient.

## design

### threads and data flow

| Thread | Rate | Does |
|---|---|---|
| accel | 100 Hz | `k_timer` tick → read 3×`int16` → push milli-g to a ring buffer |
| BLE tx | per connection interval | drain the ring → `bt_gatt_notify` |
| temp | 1 Hz | `sensor_sample_fetch` on the die sensor → notify |
| audio | on demand | capture → FFT → display |

Button events arrive on the `input` subsystem callback with no thread of their own: the
callback pushes the event to a `k_msgq` that the BLE tx thread drains, buzzes on B, and
signals the audio thread on A.

### audio pipeline

Sample rate is **31250 Hz** — Zephyr's SAADC driver takes `interval_us` as an integer, so
32 µs is the closest step to 32 kHz and it divides exactly. Nyquist is 15.6 kHz, well
above what the MEMS element can hear.

Capture is block-wise rather than one big buffer: one second of audio is 61 KB against
128 KB of total RAM, and CMSIS-DSP's `arm_rfft_fast_f32` caps at 4096 points regardless.

1. Assert the mic-enable GPIO, wait ~10 ms for the bias to settle.
2. 15 iterations of a blocking `adc_read()`, 2048 samples each (15 × 2048 / 31250 =
   0.98 s).
3. Per block: convert to `float32`, subtract the block mean, apply a Hann window, run a
   2048-point real FFT, take the magnitude, accumulate into a running average (Welch's
   method — averaging 15 spectra is what buys the noise floor).
4. Find the peak bin above ~30 Hz, then refine it with parabolic interpolation on the
   three log-magnitudes around it. Raw bin width is 15.26 Hz; interpolation gets well
   inside that.
5. Scroll the result on the LED matrix.

### GATT payloads

One custom 128-bit vendor service with three notify characteristics, each with a CCC
descriptor.

| Characteristic | Payload |
|---|---|
| Accelerometer | `uint32 t_ms` (first sample) · `uint8 count` · `int16 x,y,z` × count, milli-g |
| Temperature | `int16` centi-°C |
| Button | `uint8 button` (0 = A, 1 = B) · `uint8 state` (0 = release, 1 = press) · `uint32 t_ms` |

The accelerometer batch size is computed at runtime from the negotiated MTU —
`(bt_gatt_get_mtu(conn) - 3 - 5) / 6` — which is what "maximize throughput over each
connection interval" means in practice. It is capped at 10 samples so latency stays
bounded at 100 ms.

## building and flashing

```sh
export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -b bbc_microbit_v2 -p auto .
west flash -r pyocd
```

The micro:bit V2 exposes a DAPLink interface; `--target=nrf52833` is already in the
board's `board.cmake`. Console is on `uart0` at 115200.

## known limitations

- **The accelerometer is polled, not interrupt-driven.** P0.25 is `COMBINED_SENSOR_INT`, a
  single open-drain, *active-low* line shared by the accelerometer, the magnetometer and
  the KL27 interface chip. The `st,lis2dh` binding documents `irq-gpios` as "active-high as
  produced by the sensor" and the driver never reconfigures the chip's INT1 polarity or
  drive, so a `SENSOR_TRIG_DATA_READY` trigger arms an edge that never arrives — measured
  on hardware, P0.25 sits low indefinitely while the data-ready flag stays latched and not
  one sample is delivered. Zephyr's own board DTS declares the pin `GPIO_ACTIVE_HIGH` on
  both sensor nodes, which is what makes the trigger look like it ought to work. A 10 ms
  kernel timer is used instead; a 6-byte burst at 100 Hz is about 2 % of the I²C bus.
- **The capture is not quite gapless.** The FFT for each block runs between `adc_read()`
  calls, so roughly 4 ms of every 65.5 ms block interval is not sampled — about 6 % duty
  loss, measured as a 1056 ms wall time for 993 ms of audio. Harmless for peak detection.
  Truly gapless capture would mean bypassing the Zephyr ADC API for raw `nrfx_saadc`
  double-buffering.
- **Die temperature is not ambient temperature.** See the hardware notes above.
- **The usable audio band ends around 10 kHz**, limited by the microphone element rather
  than by the sample rate.
- **ATT MTU must be negotiated above the 23-byte default**, or the accelerometer batching
  is pointless — 20 usable bytes is only three samples. `CONFIG_BT_L2CAP_TX_MTU` and
  `CONFIG_BT_BUF_ACL_TX_SIZE` are both raised in `prj.conf`, but the negotiation is the
  central's to start. The peripheral deliberately does not call `bt_gatt_exchange_mtu()`
  itself: against BlueZ that request draws no response at all, and 30 s later the ATT
  transaction times out and takes the connection down with it. BlueZ settles on 247.
- **Unsubscribing logs one `accel notify failed (-22)`.** The transmit thread can have a
  batch in flight when the CCC is cleared. It is a benign race — the thread carries on and
  the next batch is simply not sent.
- **The P0.05 and P0.20 pin assignments come from the micro:bit hardware documentation,
  not from Zephyr**, since the board files describe no microphone at all. The
  [V2 pinmap](https://tech.microbit.org/hardware/schematic/) names them `MIC_IN` and
  `RUN_MIC`. Confirmed on hardware against a known tone, so `app.overlay` is describing
  the real microphone — but it is describing it from documentation, and a board revision
  that moved either pin would break audio capture with no build-time warning.
