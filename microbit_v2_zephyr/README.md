# bbc micro:bit v2 zephyr rtos application

Learn [Zephyr RTOS](https://docs.zephyrproject.org/latest/index.html) by building an application for the [micro:bit V2](https://docs.zephyrproject.org/latest/boards/bbc/microbit_v2/doc/index.html) board, which has several on board sensors, buttons, LEDs, and BLE communication capabilities.

This application sits in a repo outside of the standard zephyr workspace. See [zephyr application development](https://docs.zephyrproject.org/latest/develop/application/index.html#application), focusing on the "Zephyr freestanding application" pattern. The zephyr workspace is at ~/zephyrproject.

All six requirements have been run and checked on a real micro:bit V2 (board ID 9906,
i.e. V2.21 with the nRF52820 interface chip). The measurements behind that, and the three
defects the hardware turned up, are in [known limitations](#known-limitations) — most
usefully, the accelerometer's data-ready trigger does not work as Zephyr's board files
describe the pin, so it is polled.

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
| Accelerometer | LSM303AGR on I²C @0x19, INT1 → T7 → P0.25 | `lsm303agr_accel`, `compatible = "st,lis2dh"`, enabled | Poll on a `k_timer`; the trigger needs a pull-up the DTS omits |
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

Button events arrive on the `input` subsystem callback with no thread of their own, and
they do not go through a queue: the callback calls `bt_gatt_notify` itself, buzzes on B,
and requests a capture on A, all inline. The only `k_msgq` in the firmware is
`accel_msgq`. One consequence is that a button event is fire-and-forget — if nothing is
connected or the CCC is off, it is dropped rather than queued.

### audio pipeline

Sample rate is **31311 Hz**, not the 31250 that `1000000 / 32 µs` suggests. Zephyr's
SAADC driver takes `interval_us` as an integer, so 32 µs is the closest step to 32 kHz —
but `interval_to_cc()` then programs `SAMPLERATE.CC = interval_us * 16 - 1`, and the
nRF52833 samples at 16 MHz / CC. `SAMPLE_RATE_HZ` is derived from that formula rather than
assumed; see [known limitations](#known-limitations) for the measurement. Nyquist is
15.7 kHz, well above what the MEMS element can hear.

Capture is block-wise rather than one big buffer: one second of audio is 61 KB against
128 KB of total RAM, and CMSIS-DSP's `arm_rfft_fast_f32` caps at 4096 points regardless.

1. Assert the mic-enable GPIO, wait ~10 ms for the bias to settle.
2. 15 iterations of a blocking `adc_read()`, 2048 samples each (15 × 2048 / 31311 =
   0.98 s), with the HF crystal held on for the duration so the rate is the crystal's
   rather than the internal RC's.
3. Per block: convert to `float32`, subtract the block mean, apply a Hann window, run a
   2048-point real FFT, take the magnitude, accumulate into a running average (Welch's
   method — averaging 15 spectra is what buys the noise floor).
4. Find the peak bin above ~30 Hz, then refine it with parabolic interpolation on the
   three log-magnitudes around it. Raw bin width is 15.29 Hz; interpolation gets well
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

## console shell

The board runs a Zephyr shell on the same `uart0` the log uses, so it needs no extra
wiring: the V2's interface MCU presents that UART as a USB CDC device, and the host sees
it as `/dev/ttyACM0`. Anything that speaks 115200 8N1 will do.

```sh
pyserial-miniterm /dev/ttyACM0 115200      # ships in west's venv; Ctrl-] to quit
picocom -b 115200 /dev/ttyACM0             # Ctrl-A Ctrl-X to quit
tio /dev/ttyACM0                           # defaults to 115200
```

Press Enter to draw the `uart:~$` prompt. `Tab` completes commands and lists subcommands,
`-h` prints help for any of them, and log records interleave with what you type rather
than corrupting it — `CONFIG_SHELL_LOG_BACKEND` routes them through the shell, which
redraws the prompt after each one.

If the port is missing, check group membership: `/dev/ttyACM0` is usually
`root:dialout`, so either join `dialout` or rely on the udev ACL that grants the seat's
active user access (`getfacl /dev/ttyACM0` shows a `user:<you>:rw-` entry if so).

### what is available

Almost all of this comes from Zephyr rather than from application code. Only `audio` is
written here.

| Command | From | Useful for |
|---|---|---|
| `audio raw` / `audio spectrum [rows]` | `src/audio.c` | the capture path — see below |
| `audio hfxo [on\|off]` | `src/audio.c` | the HF crystal hold (on by default) |
| `input report <type> <code> <value>` | `CONFIG_INPUT_SHELL` | triggering a button without touching the board |
| `input dump on` | `CONFIG_INPUT_EVENT_DUMP` | logging every input event as it arrives |
| `log enable <level> [module]`, `log status` | `CONFIG_LOG_CMDS` | per-module verbosity at runtime |
| `sensor get <device>` | `CONFIG_SENSOR_SHELL` | reading a sensor outside the app's own threads |
| `adc adc@40007000 …` | `CONFIG_ADC_SHELL` | reading and retuning the microphone channel |
| `kernel thread stacks`, `kernel threads` | `CONFIG_KERNEL_SHELL` | stack high-water marks, thread states |
| `device list` | `CONFIG_DEVICE_SHELL` | which devices initialised |

### triggering a capture without touching the board

`buttons.c` registers its callback with `INPUT_CALLBACK_DEFINE(NULL, …)`, so it listens to
every input device and cannot distinguish a synthetic event from a real one. The whole
path runs, BLE notification included:

```
uart:~$ input report 1 30 1          # type 1 = INPUT_EV_KEY, code 30 = INPUT_KEY_A, press
uart:~$ input report 1 48 1          # code 48 = INPUT_KEY_B -- buzzes
```

This is not just convenience. Pressing A physically taps the PCB centimetres from the
microphone, and the thump lands in the very block being measured; `input report` is the
only acoustically clean way to start a capture.

### reading `audio raw`

The capture path is otherwise write-only — `accumulate_block()` builds a 1024-bin averaged
spectrum and `peak_frequency()` collapses it to the one number that reaches the LED
matrix. These two commands read the buffers that survive the capture.

```
uart:~$ audio raw
capture #1, block 15 of 15, 1061 ms elapsed (expected ~991)
2048 samples at 31311 Hz, full scale 4095 counts
  min   1713   max   1865   peak-to-peak    152 (5 mV)
  mean      1746.2 counts (63 mV)   ac rms 7.4 counts
  at rails: 0 low, 0 high
```

- **`elapsed` against `expected`** is the sample-clock check. The SAADC falls back to
  software-timed sampling if any of the three preconditions slips, silently, and the
  reported frequency is then wrong by whatever the real rate turned out to be. A few per
  cent over is the FFT time between blocks; several times over is the fallback.
- **`mean`** says whether the microphone is powered. It should idle near 1746 counts,
  43 % of full scale; with `RUN_MIC` deasserted AIN3 reads 0, so a mean near zero means the
  enable GPIO never fired.
- **`at rails`** counts samples resting on 0 or full scale — the signature of clipping.
- **`ac rms`** is the signal itself, with the DC bias removed, which is what
  `accumulate_block()` actually transforms.

Only the last of the 15 blocks is kept, so this describes the end of the capture.

### reading `audio spectrum`

```
uart:~$ audio spectrum 6
capture #1, 15 blocks averaged, 1061 ms elapsed (expected ~991)
2048-point fft at 31311 Hz, bin width 15.29 Hz, bins 2..1022 searched
interpolated peak 248.89 Hz, mean magnitude 2907.1
 rank   bin        Hz     magnitude      dBc
    1    16    244.62        6108.2      0.0
    2     2     30.58        5252.8     -1.3
    3    17    259.91        5089.0     -1.6
    4     5     76.44        5015.6     -1.7
    5   902  13790.29        4953.7     -1.8
    6   903  13805.58        4693.1     -2.3
```

That is what **no tone** looks like, and it is worth recognising: the top bin is 2.3 dB
above the sixth and only 2.1× the mean magnitude. Note that rank 1 and rank 3 are adjacent
bins around 250 Hz — that is not nothing, it is real room noise from building services,
which the channel is now sensitive enough to hear. A capture with no deliberate sound in it
shows the room, not the converter. `peak_frequency()` still returns a number — 30.52 Hz here — because it
always returns the largest bin. The `dBc` column and the ratio to `mean magnitude` are what
say whether that number means anything. A real tone puts rank 1 tens of dB clear, and its
harmonics at rank 2 and 3 land on integer multiples of its frequency.

Rows default to 8 and cap at 32. Both commands refuse to read while a capture is running,
and say so rather than returning a torn buffer.

`audio hfxo` controls whether the HF crystal is held across a capture instead of being
borrowed and released by the BLE controller. **It is held by default**; `audio hfxo off`
restores the old behaviour, which is how the sample-rate measurement in
[known limitations](#known-limitations) was made on one board without reflashing between
arms. With no argument it reports the hold and whether HFCLK happens to be running at that
instant — "running" with the hold off is normal, since advertising takes the crystal every
few tens of milliseconds.

### runtime log levels

Each module registers its own log source, so verbosity is per-subsystem:

```
uart:~$ log status                   # every module, runtime level | compiled ceiling
uart:~$ log enable dbg buttons       # button press/release tracing
uart:~$ log enable dbg accel         # "no new sample" from the poller
uart:~$ log disable temp             # silence the 1 Hz die reading
uart:~$ log enable inf               # all modules back to the default
```

The compiled ceiling is the second column and the runtime level is the first. `log enable`
cannot exceed the ceiling: with the modules registered at `LOG_LEVEL_INF`, as they
originally were, `log enable dbg accel` answers "level set to inf" and silently does
nothing. They now register at `LOG_LEVEL_DBG` and
`CONFIG_SHELL_BACKEND_SERIAL_LOG_LEVEL_INF` holds the *initial runtime* level at inf, so
the console stays quiet at boot and any single module can be raised.

### three things that will surprise you

**The `adc` shell command reconfigures the channel the application uses.** There is one
SAADC channel 3, `adc_channel_setup_dt()` configured it at init, and the shell can
overwrite it — after which the next `input report 1 30 1` capture runs at the new setting.
That is a genuine tuning lever: it is how the gain measurement in
[known limitations](#known-limitations) was made without a rebuild. Three things about it
are easy to get wrong:

- **It is `read` that applies the configuration, not the setters.** `gain`, `reference`,
  `resolution` and friends only update the shell's own copy; `adc adc@40007000 read 3` is
  what calls `adc_channel_setup()` and reaches the hardware.
- **That copy starts as zeros, so every field has to be set**, not just the one being
  changed. Miss the resolution and `read` fails with
  `ADC resolution value 0 is not valid`; miss `channel positive` and it reads the wrong
  pin. A full sequence is `resolution 12`, `acq_time 3 us`, `channel id 3`,
  `channel positive 3`, `gain …`, `reference …`, then `read 3`.
- **`audio raw`'s counts stay true across such a change; its millivolts do not**, because
  `adc_raw_to_millivolts_dt()` reads the compiled-in `app.overlay` values rather than the
  live configuration. A reset silently restores everything.

**`sensor get lsm303agr-accel@19` fails while the app is running.** It answers `Read
failed` and logs `Failed to fetch samples`. Nothing is broken: `accel.c` polls the chip at
100 Hz and consumes each data-ready, so the shell's own fetch finds the bit clear and gets
`-ENODATA` — the same case `sample_once()` treats as normal. The die sensor has no such
contention and `sensor get temp@4000c000` works.

**`accel queue 128/128, dropped …` on an idle board is expected.** Nothing drains the
queue until a BLE central subscribes, so the 10 s stats line from `main()` reports a full
queue and a rising drop count whenever nothing is connected.

### what it costs

Measured on this application, against 512 KB of flash and 128 KB of RAM:

| Build | Flash | RAM |
|---|---|---|
| no shell | 180.1 KB | 72.5 KB |
| shell tier, without `SENSOR_SHELL` | 222.7 KB | 77.8 KB |
| everything above | 238.2 KB | 84.3 KB |

`CONFIG_SENSOR_SHELL` is the expensive line: it `select`s `SENSOR_ASYNC_API`, which pulls
in RTIO, and accounts for 14 KB of flash and 6.6 KB of RAM on its own — more RAM than the
whole rest of the shell. `CONFIG_INPUT_SHELL`, by contrast, costs under a kilobyte. Drop
the sensor shell first if this ever needs to shrink.

## host side

Two programs, one pixi environment (`host/`, separate because neither bleak nor rerun is
a repo-wide dependency). `ble_stream.py` measures the link; `ble_rerun.py` visualizes it.
Both subscribe to the same three characteristics, and `ble_rerun.py` imports the UUIDs,
the `struct` formats, and the decoders from `ble_stream.py`, so the wire format above has
exactly one host-side definition.

### measuring the link

`host/ble_stream.py` subscribes to all three characteristics at once, decodes each
payload, and measures the link.

```sh
cd host
pixi run stream --seconds 20
pixi run stream --streams accel      # isolate one stream's throughput
pixi run stream --print all          # dump every decoded message, not just events
```

Button presses are not periodic, so they are printed as they happen rather than only
counted. Everything else is summarised once a second:

```
   t(s)  stream   notif/s    msg/s      B/s     bit/s   dev Hz  gap ms  bad
   14.2  accel        9.9     98.6    640.9    5126.8     99.9     101    0
         temp         1.0      1.0      2.0      15.8        -       -    0
         button       0.0      0.0      0.0       0.0        -       -    0
         TOTAL       10.8     99.6    642.8    5142.6        -       -    0
```

Reading the columns:

- **msg/s** counts what the notifications *delivered* — accelerometer samples, not
  packets, since each accel notification carries a batch of 10. Both are reported because
  the ratio between them is the batching.
- **bit/s** is the characteristic payload alone. The summary additionally reports ATT
  bytes, which add the 3-byte notification header per packet.
- **dev Hz** is derived from the device's own timestamps, so it says what the board did
  rather than how the host was scheduled. Only the first sample of a batch is stamped,
  so it is computed across whole batches.
- **gap ms** is the largest interval between consecutive batch timestamps; 101 ms against
  a nominal 100 ms is the expected quantisation.

Two things about these numbers are worth knowing before trusting them:

- **The first window always over-reads** — around 217 msg/s. Nothing drains the device's
  128-sample queue while no central is subscribed, so the first second flushes a backlog
  rather than measuring a rate. The cumulative summary carries that inflation; the
  steady-state windows do not.
- **bleak cannot report the negotiated ATT MTU on the BlueZ backend.** `client.mtu_size`
  stays at its placeholder 23 unless the characteristic is acquired, and acquiring needs
  a writable one — all three of these are notify-only. Printing it would be misleading, so
  the reader measures the consequence instead: a 65-byte accel payload is 10 samples,
  which puts the MTU at 68 or above. The firmware logs the real value (247) on subscribe.

### visualizing the streams

`host/ble_rerun.py` feeds the same decoded stream into [rerun](https://rerun.io) as live
time-series plots, modelled on `~/code/remapy/rerun_viewer/`.

```sh
cd host
pixi run viz                              # spawn the viewer, until Ctrl-C
pixi run viz --seconds 30 --window 10     # a 10 s window instead of 5
pixi run viz --no-spawn --save run.rrd    # headless; open later with `rerun run.rrd`
```

Four views. Three of them scroll with the play cursor rather than growing without bound,
each carrying a `VisibleTimeRange` built from `TimeRangeBoundary.cursor_relative`, with
`--window` setting the width (5 s by default). Only the event log is left unwindowed, so
the full history of presses stays readable.

| View | Type | Entity paths | Shows |
|---|---|---|---|
| accelerometer | `TimeSeriesView` | `accel/x`, `/y`, `/z`, `/magnitude` | m/s², one line per axis plus the magnitude |
| temperature | `TimeSeriesView` | `temp/fahrenheit` | °F |
| button state | `TimeSeriesView` | `button/state/A`, `/B` | 0 or 1 per button, held between events |
| button events | `TextLogView` | `button/raw` | one text line per notification, as received |

**Units are converted on the host, not the wire.** Milli-g becomes m/s² by multiplying by
9.80665/1000 — the exact inverse of `to_milli_g()` in `src/accel.c`, so the only loss is
the int16 quantisation of about 0.0098 m/s² per count. `accel/magnitude` is the quickest
check that this is right: it should sit near 9.81 with the board at rest. Centi-°C
becomes °F the usual way, but note this is the **nRF52833 die**, not the room — it idles
well above ambient, so 90-110 °F is normal.

The buttons get two views on purpose. The state plot uses
`SeriesLines(interpolation_mode="StepAfter")`, so the value holds until the next event
and a press and its release read as one filled interval rather than a ramp between two
points. A plotted line has no notion of "hold until the next event on this entity", so
every event restates *both* buttons and neither staircase is left with a gap; both are
also seeded at 0 when the session starts, so a button never touched still has a line.

Beside it, a `TextLogView` lists each notification as it was received — the decoded
fields next to the bytes they were unpacked from, so it doubles as a wire-format check:

```
A press  button=0 state=1 t=12345 ms  raw=00 01 39 30 00 00
A release  button=0 state=0 t=12345 ms  raw=00 00 39 30 00 00
```

Being a log list rather than a plot, it has no time axis; it keeps the whole session and
scrolls on its own.

### why button state is a plot and not a StateTimelineView

`StateTimelineView` with `rr.StateChange(state="pressed"|"released")` is the obvious fit
for this — purpose-built for state transitions, and it draws named bands instead of a 0/1
line. It was tried here and reverted, because **it cannot scroll with the time cursor in
rerun 0.36.**

Windowing is not a universal view property: it is a per-view-class opt-in,
`ViewClass::supports_visible_time_range`, and `re_view_state_timeline` does not implement
it. The failure is silent and looks like an SDK bug rather than a viewer limitation —
`VisibleTimeRanges` is accepted, stored, and written into the blueprint exactly as it is
for a plot (confirmed in the saved `.rrd`), and then ignored at render time. So do not
re-try this by checking whether the property lands; it always does.

Two things follow. `StateTimelineView` has no `time_ranges` keyword at all, which is the
first hint — it can be forced on via `view.properties["VisibleTimeRanges"]`, since that is
all `TimeSeriesView`'s keyword does underneath, but forcing it achieves nothing here. And
rerun marks both `StateTimelineView` and `StateChange` **unstable**, so this may simply
change in a later release; it is worth re-testing on a version bump, at which point the
labelled bands are the nicer view.

One thing to know about the accelerometer trace: the samples arrive ten to a
notification, and by default every sample in a batch is stamped with that
notification's arrival time, so a batch lands as ten points at one instant. That is the
honest picture of when the host learned each value. `--accel-batch-time spread`
back-dates within the batch at the firmware's 100 Hz to recover a continuous waveform —
still purely from the host clock, so no device-clock drift is folded in; it only undoes
the batching.

## known limitations

- **The accelerometer is polled, not interrupt-driven — because Zephyr's board DTS omits a
  pull-up.** On the V2 schematic the sensor's `INT1_XL` drives the base of `T7`, a
  DTC143E digital NPN whose collector is `COMBINED_SENSOR_INT` on P0.25; the
  magnetometer's DRDY drives `T5` onto the same net, as does the interface MCU. A
  common-emitter stage inverts, so the chip's default active-high INT1 is already the
  correct polarity here — the *board* is what makes the line active-low and
  open-collector, and the LSM303AGR's own polarity bit should be left alone. What is
  missing is a pull-up: nothing on the board provides one, `bbc_microbit_v2.dts` declares
  `irq-gpios = <&gpio0 25 GPIO_ACTIVE_HIGH>` without `GPIO_PULL_UP`, and the driver's bare
  `gpio_pin_configure_dt(..., GPIO_INPUT)` therefore leaves the pin floating. The first
  data-ready pulls it to 0 V, the read releases `T7`, and the floating input stays there —
  measured on hardware, P0.25 sits low indefinitely and no further edge ever arrives.
  Polarity is provably not the blocker: `int1-gpio-config` defaults to `EDGE_BOTH`, which
  is polarity-agnostic and would have caught an edge in either direction.

  An overlay would fix it:

  ```dts
  &lsm303agr_accel {
  	irq-gpios = <&gpio0 25 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
  	int1-gpio-config = <LIS2DH_DT_GPIO_INT_LEVEL_LOW>;
  };
  ```

  A 10 ms kernel timer is used anyway, because the line is genuinely shared: the interface
  MCU can assert it while the accelerometer is idle, and can hold it low across the
  sensor's own assertions. Polling sidesteps both and costs about 2 % of the I²C bus for a
  6-byte burst at 100 Hz. `CONFIG_LIS2MDL=n` is set for the same pin: the magnetometer
  driver is `default y` off the board DTS with its trigger mode defaulting to
  `GLOBAL_THREAD`, so it would otherwise arm a rising-edge interrupt on that never-rising
  pin at init, for a sensor nothing here reads.
- **The capture is not quite gapless.** The FFT for each block runs between `adc_read()`
  calls, so roughly 4 ms of every 65.5 ms block interval is not sampled — about 6 % duty
  loss, measured as a 1056 ms wall time for 993 ms of audio. Harmless for peak detection.
  Truly gapless capture would mean bypassing the Zephyr ADC API for raw `nrfx_saadc`
  double-buffering.
- **The microphone channel used to sit at `ADC_GAIN_1_4`, where the signal occupied
  2 % of the range and the FFT was mostly measuring the ADC. Fixed; this is the record.**
  Full scale was VDD, the safe choice and the wrong one for this signal. Measured with
  `audio raw` in a quiet room: powered, the channel sat at 85.8 counts of 4095 (69 mV) with
  an AC RMS of 2.3 counts; unpowered, AIN3 read about 5. `app.overlay` now asks for
  `ADC_GAIN_4` against `ADC_REF_INTERNAL`, full scale 150 mV, with `zephyr,vref-mv` moved
  to 600 to match. Against a 1000 Hz tone, nothing else changed between captures:

  | | `ADC_GAIN_1_4`, FS 3.3 V | `ADC_GAIN_4` + `INTERNAL`, FS 150 mV |
  |---|---|---|
  | AC RMS | 2.4 counts | 8.4 counts |
  | peak bin magnitude | 2253 | 41943 |
  | mean magnitude | 871 | 2816 |
  | peak above the noise floor | 4.0 dB | 18.6 dB |
  | range used, peak-to-peak | 18 of 4095 | 132 of 4095 |

  **14.6 dB of margin recovered**, while still using 3 % of the range. The bias now idles
  at about 1746 counts, 43 % of full scale, leaving roughly 29 dB of headroom before
  clipping — `audio raw`'s "at rails" counter is the thing to watch if a loud source is
  ever held against the board, and it reads 0/0 in normal use.

  What this buys is not accuracy but reach. Peak *detection* worked at either setting for
  a loud, close tone, and the interpolated frequency was about equally good. What the wide
  setting cost was everything quieter: at the old gain a 3 kHz tone played across a desk
  could not be picked out of the room at all, and at the new one it resolves to −0.011 %.
  The flip side is that the noise floor is now a real measurement of the room rather than
  of the converter, so a quiet capture will show whatever the room is actually doing —
  a 244 Hz peak from building services, in the case above.

- **`SAMPLE_RATE_HZ` used to be wrong about the hardware, in two independent ways. Both
  are fixed; this is the record of why.** The measurement uses `audio spectrum` against a
  tone at exactly a bin centre, played from a host sound card. A tone on a bin centre
  should peak *on that bin* with symmetric neighbours, so where it actually peaks measures
  the sample rate directly. Two runs per arm, 1525.88 Hz (bin 100 at the old nominal rate):

  | | peak bin | interpolated | implied rate | error |
  |---|---|---|---|---|
  | as originally written | 99 | 1515.37 Hz | 31467 Hz | −0.689 % |
  | HF crystal held on | **100** | 1522.70 Hz | 31315 Hz | −0.208 % |
  | 16 MHz / CC with CC = 511 | — | 1522.90 Hz predicted | 31311 Hz | −0.196 % |

  **The larger share was the clock behind the timer.** `SAMPLERATE.CC` counts PCLK16M,
  which derives from HFCLK, and HFCLK runs from the internal RC oscillator unless
  something asks for the crystal. The BLE controller asks around each radio event
  (`z_nrf_clock_bt_ctlr_hf_request`) and drops it again, so a capture was stitched together
  from crystal-accurate and RC-accurate stretches. `run_capture()` now wraps itself in
  `clock_control_on(hfclk, NULL)`, which moved the peak onto the correct bin and removed
  two thirds of the error. `audio hfxo off` restores the old behaviour for re-measuring.

  **The rest was a divider off-by-one.** `interval_to_cc()` in
  `drivers/adc/adc_nrfx_saadc.c:503` returns `(interval_us * 16) - 1`, so 32 µs programs
  CC = 511, not 512, and the nRF52833 samples at 16 MHz / CC = 31311.15 Hz. `SAMPLE_RATE_HZ`
  is now derived as `16000000 / (SAMPLE_INTERVAL_US * 16 - 1)` instead of the assumed
  `1000000 / SAMPLE_INTERVAL_US`. **Keep it derived**: the rate and the interval are one
  knob, and hardcoding either lets them drift apart silently.

  Verified afterwards across the band, at a gain that clears the noise floor:

  | played | 500 Hz | 1000 Hz | 3000 Hz | 5000 Hz | 8000 Hz |
  |---|---|---|---|---|---|
  | reported | 499.81 | 1000.17 | 2999.86 | 4999.98 | 7998.07 |
  | error | −0.038 % | +0.017 % | −0.005 % | −0.000 % | −0.024 % |

  Everything is inside ±0.04 % and the residuals scatter around zero rather than all
  leaning one way, which is the point: what remains is interpolation noise, not bias. At
  3 kHz the error went from −0.68 % to −0.005 %.

  A note on reproducing this: the 5 kHz and 8 kHz rows were not repeatable on a later
  attempt, where those two tones failed to register at all while 500–3000 Hz still read
  correctly. Nothing about the firmware changed between the two — high frequencies are far
  more directional than low ones, and the board had been handled between sessions. If the
  upper rows will not reproduce, aim the microphone at the source before suspecting the
  code, and check `audio spectrum 32` for the expected bin before trusting a null result.

  One caveat on the mechanism: the 16 MHz / CC semantics are inferred from the prediction
  matching the measurement to 0.013 %, not read off the nRF52833 datasheet. The agreement
  is tight, but if you are ever tempted to report the `-1` upstream as a bug, read the
  `SAMPLERATE` register description first — it may be a deliberate convention.
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
