# adafruit feather sense zephyr application

Learn [Zephyr RTOS](https://docs.zephyrproject.org/latest/index.html) by building a
sensor-streaming application for the [Adafruit Feather Bluefruit
Sense](https://learn.adafruit.com/adafruit-feather-sense/) (nRF52840), which carries a
6-axis IMU, a magnetometer, humidity, pressure, light/gesture and PDM microphone parts on
one I²C bus, plus a NeoPixel, a user button, and a battery divider.

This application sits in a repo outside of the standard zephyr workspace. See [zephyr
application development](https://docs.zephyrproject.org/latest/develop/application/index.html#application),
focusing on the "Zephyr freestanding application" pattern. The zephyr workspace is at
~/zephyrproject.

There is a prior art for this board that is worth reading first:
`~/code/remapy/adafruit_feather_sense/` is a CircuitPython application streaming the same
IMU and magnetometer over the same two transports, with measurements for most of the
decisions below. Where this document quotes a number **and does not say it was measured
here**, that is where it came from.

## status

**Implemented, built, flashed and run on hardware.** The firmware is in `src/`
and `drivers/`, the host programs in `host/`, and the `native_sim` tests in
`tests/`. What has been exercised on a real board, and what has not, is listed
at the end under [known limitations and open
questions](#known-limitations-and-open-questions) — that separation is this
document's main value and it is kept up.

Measured on the board, over USB CDC, with a 208 Hz IMU, a 20 Hz magnetometer and
a 1 Hz environmental stream all running at once:

| | |
|---|---|
| IMU rate, from device timestamps | **208.4 samples/s** (the chip's ODR is 208) |
| Magnetometer rate | **20.0 samples/s** |
| Decode errors and sequence gaps | **0** in every run, all five streams at once |
| FIFO overruns | **0**, after the IMU thread learned to flush the boot backlog |
| IMU WHO_AM_I | **0x69 — this board carries the LSM6DS33**, not the LSM6DS3TR-C |
| Accelerometer magnitude at rest | 9.99 m/s² |
| SHT30 | 26.5 °C, 54.2 %RH, and a fetch that blocks **397 µs** in steady state — not the ~152 ms this document feared |
| Battery | 4056 mV, 85 %, USB flag set, from the pack on the board |
| Magnetometer | **48.3 µT** total field — Earth's, once the board was moved away from a magnet |
| Status LED | green at a `high` band, on `P0.16` — **confirmed**, once the bit-bang timing was fixed |
| IMU drain | the chip's own FIFO-watermark interrupt, on **`P1.11`** — every batch exactly 10 samples, 1251 of 1251 |
| Flash / RAM | 236 948 B (29 %) / 79 048 B (30 %) |

The same, over BLE, with all four notify characteristics subscribed:

| | |
|---|---|
| IMU rate, from device timestamps | **208.4 samples/s** — the same as USB |
| Decode errors, sequence gaps, device-side notification drops | **0** over a 20 s run |
| Largest notification observed | **130 bytes**, so the ATT MTU is at least 133 — a fixed size now that every IMU batch is exactly the watermark |
| ATT MTU the board negotiated | **247**, from the board's own log |

So both transports carry the full 208 Hz stream with nothing dropped, which was
the point of the whole design. The link is nowhere near saturated: about
2.7 KB/s of samples against the ~4.4 KB/s the CircuitPython port reached at a
23-byte MTU.

Building the firmware turned up ten things this document had wrong or had not
known, all of them recorded in place below rather than only here:

1. The image linked at flash offset 0 and would have erased the SoftDevice
   and the MBR the bootloader itself needs — see [building and
   flashing](#building-and-flashing).
2. Zephyr's boot-time CDC ACM initializer registers only the *first* instance,
   so the data port was built and never enumerated — see [the second CDC ACM
   instance](#the-second-cdc-acm-instance).
3. `CONFIG_I2C_NRFX_TWIM` is a third promptless symbol and cannot be asserted in
   `prj.conf`.
4. The battery divider *does* need an overlay: the board declares `vbatt` but no
   ADC channel to go with it, and the result is a silent 0 mV — see [battery and
   the status led](#battery-and-the-status-led).
5. Zephyr's SHT30 driver runs in **periodic** mode by default, not the blocking
   single-shot conversion assumed here, which voids this document's largest
   performance worry — see [the environmental read](#the-environmental-read).
6. The magnetometer's wire unit clipped before the sensor did, on the very first
   board — see [how values are encoded](#how-values-are-encoded).
7. The scale table is in *nano*-SI, so a dimensionless field's identity is
   `1e9/1`. Writing `1/1` made a working light sensor report `0.0000`.
8. Advertising cannot be restarted from the `disconnected` callback, so the
   board advertised exactly once per boot — see [restarting
   advertising](#restarting-advertising).

9. The NeoPixel's bit-bang delays fell through to literals meant for a ~10 MHz
   CPU, because nRF52840's dtsi declares no `cpu@0` clock frequency. The pixel
   lit in the *wrong colour* rather than staying dark, which points at the colour
   mapping and away from the real cause — see [the status led's bit-bang
   timing](#the-status-leds-bit-bang-timing).
10. The IMU's INT1 **is** routed, to `P1.11`, and this document's own suggested
   way of finding it would not have worked — a watermark interrupt pulses, and a
   shell reads one pin at a time. See [the imu's INT1
   line](#the-imus-int1-line).

And one thing the board corrected about the *bench* rather than about the
firmware: the magnetometer's first readings were saturated on all three axes,
which looked like a fault and was not — see [the
magnetometer](#the-magnetometer).

## requirements

1. Sample the sensors and stream them to a host machine for processing, visualization,
   and recording. In descending order of desired data rate:
    1. accelerometer
    1. gyroscope
    1. magnetometer
    1. temperature
    1. humidity
    1. light level
    1. battery percent — only transmitted on a change of at least 1 %
    1. user button — press and release events
1. Sample with interrupts wherever possible. Minimize CPU blocking. Use DMA where
   possible to route data from the external sensors into transport buffers.
1. Two transport methods, which may be separate applications:
    1. USB serial, using a combined packet of data to maximize throughput.
    1. BLE characteristics with streaming notifications. Separate sensors with the same
       sampling rate into separate characteristics, and buffer multiple samples to
       maximize throughput while maintaining low latency for real-time processing.
1. Data encoding:
    1. Only fixed-width values on the wire, no floating point. The host decodes
       transmitted values into standard units.
    1. Include a millisecond timestamp counter for each batch of values transmitted
       together.
1. Remote procedure calls, so the host can query the device and change its behavior:
    1. get battery level
    1. enable streaming of each input type
    1. get scaling factors for specific streaming input types
    1. get board serial number
    1. get firmware build ID
1. Battery status LED: set the onboard LED to a color corresponding to battery level
   low, medium, or high.
1. Host applications:
    1. CLI applications to read streamed data rates over USB serial and over BLE.
    1. A rerun program to visualize inputs and sensors in real time.

Future, out of scope here: processing with a `dfg` graph, and recording HDF5 and/or
parquet files.

## hardware notes

The gap between what the board *has* and what Zephyr's board support *enables* is wider
here than on the micro:bit, and it is the main thing to understand before reading
anything else. Verified against Zephyr 4.4.99.

| Input | Chip | I²C | Zephyr driver | Status |
|---|---|---|---|---|
| Accelerometer + gyroscope | LSM6DS33 (**confirmed**, `WHO_AM_I` 0x69) | `0x6a` | **none in tree** | Out-of-tree driver in `drivers/lsm6ds3trc/` |
| Magnetometer | LIS3MDL | `0x1c` | `st,lis3mdl-magn` | Driver exists; needs an overlay node |
| Temperature + humidity | SHT30 | `0x44` | `sensirion,sht3xd` | **Already in the board DTS** (`sht3xd@44`) |
| Light level | APDS9960 | `0x39` | `avago,apds9960` | Driver exists; needs an overlay node |
| Battery | divider to `AIN5`, 100k/200k | — | `voltage-divider` | Node in the board DTS (`vbatt`), but the ADC **channel** is not — see below |
| User button | switch on `P1.02`, active low | — | `gpio-keys`, `INPUT_KEY_0`, alias `sw0` | **Already in the board DTS** |
| Status LED | NeoPixel on `P0.16` (**confirmed**) | — | `worldsemi,ws2812-gpio` | Needs an overlay node, and a CPU clock frequency — see below |
| Pressure | BMP280 | `0x77` | `bosch,bme280` (accepts chip id `0x58`) | Available, **not used** |
| Microphone | PDM MEMS | — | `nordic,nrf-pdm` | Available, **not used** |

`i2c scan i2c@40003000` on the board finds exactly five: `0x1c`, `0x39`,
`0x44`, `0x6a` and `0x77`. That is the table above, minus the microphone, which
is not an I²C part.

Four consequences worth stating outright.

**Zephyr has no driver for this board's IMU.** `drivers/sensor/st/` ships `lsm6ds0`,
`lsm6dsl`, `lsm6dso`, `lsm6dso16is` and the `lsm6dsv*` family. The LSM6DS33 and the
LSM6DS3TR-C are in none of them. The nearest fit is `st,lsm6dsl`, whose `WHO_AM_I` check
expects `0x6a` — which the LSM6DS3TR-C happens to answer, and the LSM6DS33 (`0x69`) does
not — but that driver offers data-ready triggers only, with **no FIFO support at all**, so
it cannot serve requirement 3.2's batching from a hardware-clocked source. This board turns
out to carry the **LSM6DS33**, so `st,lsm6dsl` would have rejected it outright. What *is*
available is ST's own register-level driver, already vendored in the west workspace at
`modules/hal/st/sensor/stmemsc/lsm6ds3tr-c_STdC/driver/` and wired into the build at
`stmemsc/CMakeLists.txt:69` behind `CONFIG_USE_STDC_LSM6DS3TR_C`. That symbol has **no
prompt** (`modules/hal_st/Kconfig:168` declares a bare `bool`), so it cannot be set from
`prj.conf` — setting it there is a hard configuration error. The out-of-tree driver's own
Kconfig must `select USE_STDC_LSM6DS3TR_C`, exactly as
`drivers/sensor/st/lsm6dso/Kconfig:18` selects its own. It has the full FIFO
API (`lsm6ds3tr_c_fifo_mode_set`, `..._watermark_set`, `..._raw_data_get`). So this
application carries a small app-local Zephyr sensor driver over that vendored code — a
bus shim, an init path, and a FIFO-watermark trigger. No third-party code is copied into
this repo.

**The board's I²C is the non-DMA peripheral.** `adafruit_feather_nrf52840_common.dtsi:92`
declares `&i2c0` with `compatible = "nordic,nrf-twi"`. TWI is the legacy, register-at-a-
time peripheral; TWIM is the EasyDMA one. Requirement 2's "use DMA where possible" is
answered on this board by one overlay line changing that compatible, plus raising the
clock. The CircuitPython port measured 1.8×–1.85× per read going from 100 kHz to 400 kHz
on exactly these parts.

**The board DTS describes one of the six sensors.** Only the SHT30 has a node. The IMU,
magnetometer, light sensor and NeoPixel are all invisible to devicetree until this
application's `app.overlay` declares them. Two Kconfig defaults are traps once they are
declared: `CONFIG_LIS3MDL_ODR` defaults to the string `"0.625"` — 0.625 Hz, against the
20 Hz this application wants — and `CONFIG_LIS3MDL_FS` defaults to `4`, meaning ±4 gauss
at 6842 LSB/gauss. Both are set explicitly in `prj.conf` rather than inherited. This is
the Zephyr form of the CircuitPython port's rule that **output data rate is set, never
inherited**; there it was a runtime call, here it is a build-time symbol, and the failure
mode of getting it wrong (re-reading samples the chip has not refreshed) is identical.

### the second CDC ACM instance

**The console is already on USB, and that is a problem the board solves for free.** The
`sense/uf2` board variant includes `boards/common/usb/cdc_acm_serial.dtsi`, which creates
a `board_cdc_acm_uart` CDC ACM instance and chooses it for `zephyr,console` and
`zephyr,shell-uart`. The CircuitPython port's single most annoying defect was that its
console *was* its data channel, so the runtime's status-bar escape sequence landed
between two binary frames on every host attach. Here the overlay declares a **second**
CDC ACM instance for data, and log output can never reach it. That is a structural fix,
not a workaround.

**It does not work for free, though.** That same board fragment turns on
`CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT`, whose initializer at
`subsys/usb/device_next/app/cdc_acm_serial.c` says in its own comment: *"This code only
registers the first CDC-ACM instance."* Left in charge, it builds the application's
`cdc_acm_data` instance, binds a Zephyr device to it, and never enumerates it — so writes
to the data port succeed and go nowhere, which is the worst shape a fault can take. The
fix is to turn the boot initializer off in `prj.conf` and do the usbd setup in
`src/usb.cpp` with `usbd_register_all_classes()`, which takes both. The host then sees two
ACM devices, and `lsusb -v` shows both interfaces.

**Which `/dev/ttyACM*` is which is answered by descriptor, not by order.** The
`zephyr,cdc-acm-uart` binding's `label` property *becomes the USB interface string
descriptor*, so the overlay names them "Feather Sense console" and "Feather Sense data"
and Linux exposes the string at `/sys/class/tty/ttyACMn/device/interface`.
`host/read_serial.py`'s `find_port()` resolves the data port that way and refuses to guess
if it cannot. On the board this ran on the data port came up as `ttyACM1` and the console
as `ttyACM0`, but nothing depends on that.

## design

### devicetree overlay

`app.overlay`, in full — this is the file, not a paraphrase of it:

```dts
/*
 * The board DTS declares exactly one of the six sensors on the internal I2C
 * bus (the SHT30). The IMU, the magnetometer, the light sensor and the
 * NeoPixel are invisible to devicetree until they are declared here.
 *
 * See README.md, "hardware notes".
 */

#include <zephyr/dt-bindings/adc/adc.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/dt-bindings/i2c/i2c.h>
#include <zephyr/dt-bindings/led/led.h>

&i2c0 {
	/* EasyDMA. adafruit_feather_nrf52840_common.dtsi gives this bus
	 * "nordic,nrf-twi", the legacy register-at-a-time peripheral; TWIM is
	 * the same pins with DMA behind them. This one line is the whole of
	 * requirement 2's "use DMA where possible" on this board. Confirm it
	 * took with `grep CONFIG_I2C_NRFX_TWIM build/zephyr/.config`.
	 */
	compatible = "nordic,nrf-twim";
	clock-frequency = <I2C_BITRATE_FAST>;	/* 400 kHz */

	imu: lsm6ds3trc@6a {
		/* Out-of-tree; see drivers/lsm6ds3trc/. Zephyr ships no driver
		 * for either part this board has carried.
		 */
		compatible = "scratchpad,lsm6ds3trc";
		reg = <0x6a>;
		/* INT1, carrying the FIFO watermark. Nothing documents this
		 * pin -- it was found by driving INT1 statically high and
		 * reading every free GPIO, see README.md, "the imu's INT1
		 * line". The property alone does not switch the trigger on:
		 * the driver's Kconfig choice defaults to
		 * LSM6DS3TRC_TRIGGER_NONE, so prj.conf selects GLOBAL_THREAD
		 * as well.
		 */
		irq-gpios = <&gpio1 11 GPIO_ACTIVE_HIGH>;
	};

	magn: lis3mdl@1c {
		compatible = "st,lis3mdl-magn";
		reg = <0x1c>;
		/* No irq-gpios: selects CONFIG_LIS3MDL_TRIGGER_NONE. */
	};

	light: apds9960@39 {
		compatible = "avago,apds9960";
		reg = <0x39>;
		/* No int-gpios: selects CONFIG_APDS9960_FETCH_MODE_POLL. */
	};
};

/*
 * The battery divider's ADC channel.
 *
 * The board dtsi declares the `vbatt` voltage-divider node itself -- ADC
 * channel 5, output-ohms 100k, full-ohms 200k -- but it declares no `channel@5`
 * under &adc to go with it, and without one ADC_DT_SPEC_GET() yields a spec
 * with channel_cfg_dt_node_exists = false and vref_mv = 0. Nothing fails
 * loudly: adc_channel_setup_dt() configures no channel, the `adc` shell
 * answers "Channel 5 not configured", and adc_raw_to_millivolts_dt() multiplies
 * by a zero reference and reports 0 mV forever. Measured on hardware; the
 * design document had claimed the divider needed no overlay at all.
 */
&adc {
	#address-cells = <1>;
	#size-cells = <0>;

	channel@5 {
		reg = <5>;
		/* The divider halves the pack, so a 4.2 V cell presents 2.1 V.
		 * Gain 1/6 against the 0.6 V internal reference puts full scale
		 * at 3.6 V, which covers that with room to spare and is the
		 * usual nRF52 battery arrangement. */
		zephyr,gain = "ADC_GAIN_1_6";
		zephyr,reference = "ADC_REF_INTERNAL";
		/* The reference itself, not the full-scale input.
		 * adc_raw_to_millivolts_dt() reads this through DT_PROP_OR(...,
		 * 0), so omitting it costs a wrong answer rather than a build
		 * error -- every reading converts to 0 mV. */
		zephyr,vref-mv = <600>;
		/* 100k in parallel with 100k is a 50k source, which needs a
		 * long sample window; 40 us is the SAADC's maximum. */
		zephyr,acquisition-time = <ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40)>;
		zephyr,input-positive = <NRF_SAADC_AIN5>;
		zephyr,resolution = <12>;
	};
};

/*
 * The CPU clock, which the NeoPixel's bit-banged timing is derived from.
 *
 * `worldsemi,ws2812-gpio` has no clock of its own: it toggles the line with
 * inline assembly and counts NOPs, and Kconfig works out how many from
 * /cpus/cpu@0's clock-frequency. nrf52840.dtsi does not declare one (nrf52810
 * and nrf52805 do), and neither did this overlay, so CONFIG_DELAY_T1H and its
 * three siblings fell all the way through to their last-resort literals -- 7,
 * 6, 3 and 8 NOPs, which are values for a roughly 10 MHz part. At 64 MHz that
 * made a "1" bit's high pulse about 109 ns where the WS2812 wants 700, and the
 * LED decoded garbage: it lit, in the wrong colour, which is a far more
 * confusing failure than staying dark.
 *
 * Declaring the frequency here is what the driver's own binding does in its
 * first example, and it gives 44/38/22/51 NOPs. Nothing else on this SoC reads
 * the property -- the drivers that do are all for other vendors' parts.
 * Check it took: `grep CONFIG_DELAY_T build/zephyr/.config`.
 */
&{/cpus/cpu@0} {
	/* Path syntax, not a label: nrf52840.dtsi gives the node none. */
	clock-frequency = <64000000>;
};

/ {
	neopixel: ws2812 {
		compatible = "worldsemi,ws2812-gpio";
		gpios = <&gpio0 16 0>;
		chain-length = <1>;
		color-mapping = <LED_COLOR_ID_GREEN
				 LED_COLOR_ID_RED
				 LED_COLOR_ID_BLUE>;
	};

	aliases {
		imu0 = &imu;
		magn0 = &magn;
		light0 = &light;
	};
};

/*
 * A second CDC ACM instance, for binary sample data only. The board's own
 * `board_cdc_acm_uart` (from boards/common/usb/cdc_acm_serial.dtsi, which the
 * sense/uf2 variant includes) keeps the console, the shell and the log, so the
 * two can never share a pipe. This is the structural fix for the defect that
 * cost the CircuitPython port a trustworthy error counter.
 */
&zephyr_udc0 {
	cdc_acm_data: cdc_acm_data {
		compatible = "zephyr,cdc-acm-uart";
		/* The binding's `label` becomes the USB *interface string
		 * descriptor*, which is how the host tells the two ACM ports
		 * apart without assuming enumeration order: on Linux it lands
		 * in /sys/class/tty/ttyACMn/device/interface. host/ resolves
		 * the port by this string. See host/read_serial.py, find_port().
		 */
		label = "Feather Sense data";
	};
};

/* Same treatment for the board's own console instance, so neither port has to
 * be identified by elimination. */
&board_cdc_acm_uart {
	label = "Feather Sense console";
};
```

Two things in there are corrections to what this document originally claimed.

**The battery divider does need an overlay after all.** The board dtsi declares the
`vbatt` node — a `voltage-divider` on `&adc 5` with `output-ohms = <100000>` and
`full-ohms = <200000>` — but it declares no matching `channel@5` under `&adc`, and
`ADC_DT_SPEC_GET()` needs one for gain, reference, acquisition time and `vref-mv`.
Without it nothing fails loudly: `adc_channel_setup_dt()` configures no channel, the
`adc` shell answers `Channel 5 not configured`, and `adc_raw_to_millivolts_dt()`
multiplies by a `DT_PROP_OR(..., 0)` reference and reports **0 mV forever**. `fs battery`
read `0 mV 0 %` on the first firmware and that is why.

**The `label` properties are load-bearing**, not documentation: the binding turns them
into USB interface string descriptors, which is what lets the host resolve the data port
without assuming enumeration order. See [the second CDC ACM
instance](#the-second-cdc-acm-instance).

The user button still needs no overlay: the board dtsi has a `gpio-keys` `button0` on
`gpio1 2` with `zephyr,code = <INPUT_KEY_0>` and the `sw0` alias, so the button goes
through the `input` subsystem with no debounce code and no polling loop of our own.

### threads and data flow

| Thread | Prio | Woken by | Rate | Does |
|---|---|---|---|---|
| imu | 5 | FIFO watermark IRQ on `P1.11` | 208 Hz | drain N records → one batch → both tx queues |
| magn | 6 | DRDY trigger, else `k_timer` | 20 Hz | fetch → convert → batch |
| tx ble | 7 | `k_msgq` | per connection interval | `bt_gatt_notify` per stream |
| tx usb | 7 | `k_msgq` | as drained | COBS-encode → write to `cdc_acm_data` |
| env | 8 | `k_timer` | 1 Hz | SHT30 + APDS9960 fetch |
| battery | 9 | `k_timer` | 1 Hz | ADC read; emit only on a ≥1 % change; repaint the LED on a band change |

Button events arrive on an `input` subsystem callback with no thread of their own, as in
the micro:bit application, and are pushed straight onto both tx queues.

Two notes on the priorities. The env thread is the lowest of the sampling threads
deliberately, on the assumption that its read was expensive; see [the environmental
read](#the-environmental-read) for what that turned out to cost. The tx threads sit above
env and battery so a full queue drains ahead of new low-rate work.

Everything is threads and message queues; there is no `k_work` anywhere, matching both
existing Zephyr areas. Three threads not in the table serve the transports rather than the
sensors: `tx_ble` and `tx_usb` drain the two queues, and `usb_rx` reassembles COBS frames
from the interrupt handler's ring buffer and answers RPC requests. Every RPC opcode
answers from cached state — `get battery` reads the battery thread's last sample rather
than the ADC — so a request never blocks the thread it arrived on, which is what lets a
GATT write be answered inline on the Bluetooth RX thread.

### the environmental read

This document's largest performance worry was that Zephyr's `sht3xd` driver "does a
blocking single-shot conversion", quoting the CircuitPython port's ~152 ms per second as
an upper bound to check against. **The premise was wrong.** The driver's measurement-mode
choice defaults to `CONFIG_SHT3XD_PERIODIC_MODE` at one measurement per second, not to
`SHT3XD_SINGLE_SHOT_MODE` — so a fetch is a `FETCH_DATA` command and a six-byte read, not
a conversion, and `fs env` reports it in microseconds because milliseconds could not
resolve it.

The measured cost is **397 µs**, repeatably, once the application has settled; the first
fetch after boot has been seen at 2.7 ms. Either way it is three orders of magnitude below
the figure this design was built to defend against, and the env thread's low priority is
now insurance rather than necessity.

Periodic mode brings its own trap, which the single-shot assumption hid. The chip NACKs
`FETCH_DATA` when no new measurement is ready, so a 1 Hz reader against a 1-measurement-
per-second chip loses the race whenever it drifts ahead, and the driver reports
`Failed to fetch samples`. `CONFIG_SHT3XD_MPS_2=y` gives the chip twice the reader's rate
so there is always one waiting. `fs env` counts the failures, and it should read 0.

### the magnetometer

**The first readings were saturated on all three axes at once, and that was the bench, not
the board.** It is worth recording how it was settled, because "the sensor is broken" was
the obvious reading and it was wrong.

The symptoms all pointed the wrong way. With `CONFIG_LIS3MDL_FS=4` the raw range tops out
at ±32767 LSB ≈ ±479 µT per axis, and every axis sat within 70 counts of that — with a
standard deviation *below one LSB*, where the part's own spec noise is ~0.32 µT. Low noise
at full scale is what a pinned ADC looks like, and a single external field vector does not
usually saturate three orthogonal axes equally.

The decisive test was one register write. Widening the full scale to ±16 gauss through the
`i2c` shell and re-reading `OUT_X_L` onward:

| axis | at ±4 gauss | at ±16 gauss |
|---|---|---|
| X | −32686 (railed) | −31393 = **−18.4 gauss** |
| Y | +32681 (railed) | +32698 (still railed, so > 19.1 gauss) |
| Z | −32708 (railed) | −13265 = **−7.75 gauss**, off the rail |

Z came off the rail and returned a real value. A faulty or pinned sensor does not track a
range change; this one did. So the field was genuine, over 27 gauss in total — magnet
territory, and the board was sitting next to one.

Moving the board settled it completely: **48.3 µT total**, with per-axis noise of 0.4–0.7
µT against a 0.32 µT spec. That is Earth's field, and it is the number a magnetometer on a
desk should report.

Two things worth keeping from this. The deci-µT wire unit gives Earth's field about 480
counts, so there is resolution to spare and no risk of the wire clipping before the sensor
— which was the reason for the change. And a sensor that saturates is not necessarily a
sensor that is broken: the cheapest way to tell the two apart is to move the range and see
whether the reading moves with it.

### the imu's INT1 line

**`P1.11`, found by sweep rather than from a schematic.** Nothing describes this pin.
Zephyr's board files do not mention the IMU at all, Adafruit's pinout diagram covers the
header and not the internal bus, and the CircuitPython port never used an interrupt, so it
left the question open too. This document listed it as unverified for exactly that reason,
and suggested "arm the watermark interrupt and sweep the candidate GPIOs".

Taken literally that does not work. A watermark interrupt *pulses*, twenty times a second,
and a shell command that reads one pin at a time will essentially never catch a pulse. The
sweep needs INT1 held **statically** high, and the LSM6DS33 offers exactly that if the
source is chosen carefully: `XLDA` is set when a new accelerometer sample is ready and
cleared only by reading `OUTX_L_XL` — registers **this firmware never touches**, because it
reads the FIFO instead. So `INT1_CTRL = 0x01` (`INT1_DRDY_XL`) raises INT1 and leaves it
raised.

The reading side is `CONFIG_DEVMEM_SHELL`, which was already on. Writing `0x4` to a pin's
`PIN_CNF` makes it an input **with a pull-down**, so a floating header pin reads 0 and only
a pin something is actively driving reads 1; `P0.IN` (`0x50000510`) and `P1.IN`
(`0x50000810`) then hand back all 32 lines of a port per read. Sweeping the 27 pins the
board, this overlay and the SoC do not already own:

| `INT1_CTRL` | routed source | `P1.IN` | high among the candidates |
|---|---|---|---|
| `0x00` | nothing | `0x00000004` | none |
| `0x01` | accelerometer data-ready | `0x00000804` | **`P1.11`** |
| `0x00` | nothing | `0x00000004` | none |
| `0x02` | gyroscope data-ready | `0x00000804` | **`P1.11`** |

One pin, following two independent sources, going low again each time the routing is
removed. (The `0x4` present in every row is `P1.02`, the user button, idling high on its
pull-up; it is not a candidate.) The same sweep against `INT2_CTRL` found nothing, so INT2
is not routed — which is why the binding has one `irq-gpios` entry and not two.

**Turning it on takes two changes, not one.** `irq-gpios = <&gpio1 11 GPIO_ACTIVE_HIGH>` in
`app.overlay` only makes the trigger *available*: the driver's Kconfig choice defaults to
`LSM6DS3TRC_TRIGGER_NONE`, so `prj.conf` has to select
`CONFIG_LSM6DS3TRC_TRIGGER_GLOBAL_THREAD=y` as well. With one and not the other the board
boots and logs `INT1 trigger refused; falling back to the timer` — `sensor_trigger_set()`
answers `-ENOSYS`, because the driver's `.trigger_set` is then NULL — which is
`src/imu.cpp`'s fallback branch exercised for free. `draining the FIFO on the INT1
watermark` at boot is the line that says it took.

#### what the trigger actually bought

Less than "interrupts beat polling" would suggest, and the honest number is the point of
measuring. The timer period was already the watermark's cadence, so the win is not
throughput and it is barely latency; it is **determinism**. Both builds, sixty seconds
each, all five streams running:

| | `k_timer`, 49 ms | INT1 watermark |
|---|---|---|
| IMU batches in 60 s | 1224 | 1251 |
| … carrying 10 samples | 958 | **1251** |
| … carrying 11 | 266 — **21.7 %** | **0** |
| device-side batch interval, min/median/max | 47.2 / 49.0 / 50.8 ms | 46 / 48 / 49 ms |
| IMU rate from device timestamps | 208.5/s | 208.5/s |
| host-side decode errors and `seq` gaps | 0 | 0 |

That 21.7 % is not noise, it is a beat, and it was predictable: a 49 ms timer against a
watermark that fills in 10 / 208.5 s = 47.96 ms runs 1.04 ms slow per drain, so
(49 − 47.96) / 4.796 = **21.7 %** of drains should find an extra sample waiting. The
measurement lands on the arithmetic exactly. Nothing was ever *wrong* with that — the drain
loop empties the FIFO however deep it finds it, and neither build lost a sample — but the
batch boundary wandered against the sensor's clock, and now it does not. On the interrupt
build `fs imu` reports 20 420 samples in 2042 batches with **0 overruns and 0 stall
flushes**.

Two consequences worth recording. The largest BLE notification is now a fixed 130 bytes
(10 samples × 12, plus the 10-byte header) rather than an occasional 142, which lowers the
observed-MTU floor in the [status](#status) table without saying anything worse about the
link — the board's own negotiated 247 is unchanged. And the trigger's work item runs on
`sysworkq`, which reports 376/1024 bytes. The whole thing costs 984 bytes of flash.

### the status led's bit-bang timing

**The pixel lit in the wrong colour, and the cause was neither the pin nor the colour
mapping.** `worldsemi,ws2812-gpio` has no clock of its own: it toggles the line with inline
assembly and counts NOPs, and Kconfig works out how many from `/cpus/cpu@0`'s
`clock-frequency`. Two things have to be true for that to happen, and neither was:

```
config DELAY_T1H
	default $(dt_node_int_prop_int,$(DT_CHOSEN_LED_STRIP_PATH),delay-t1h)
		  if $(dt_node_has_prop,$(DT_CHOSEN_LED_STRIP_PATH),delay-t1h)
	default $(div,$(mul,700,$(dt_node_int_prop_int,/cpus/cpu@0,clock-frequency)),1000000000)
		  if $(dt_node_has_prop,/cpus/cpu@0,clock-frequency)
	default 7
```

There is no `zephyr,led-strip` chosen node and no `delay-t*` properties, and
**`nrf52840.dtsi` declares no `clock-frequency` on `cpu@0`** — `nrf52810.dtsi` and
`nrf52805.dtsi` do, which is what makes the omission easy to miss. So all four delays fell
through to their last-resort literals, 7/6/3/8 NOPs, which are values for a roughly 10 MHz
part. At 64 MHz a NOP is about 15.6 ns, so a "1" bit's high pulse came out at ~109 ns
against the WS2812's ~700 ns, and the whole bit period was about a fifth of the 1.25 µs the
part expects.

The LED therefore latched garbage — and, importantly, **lit while doing it**. A dark pixel
would have pointed straight at the pin. A lit pixel in the wrong colour points at the
colour mapping, which was correct all along.

The fix is one line in `app.overlay` declaring the frequency the SoC actually runs at,
which yields 44/38/22/51 and makes the pixel read green at a battery band of `high`.
Nothing else on this SoC reads that property; the drivers that do are all for other
vendors' parts.

```sh
grep CONFIG_DELAY_T build/zephyr/.config     # 44/38/22/51, not 7/6/3/8
```

Two things this settles as a side effect. **The NeoPixel is on `P0.16`** — inherited from
the Adafruit pinout and unverified until something lit. And the `color-mapping` in the
overlay is right: GRB, as the part expects.

`fs led <r> <g> <b>` exists because of this. A NeoPixel has no readback, so the only way to
separate a timing fault from a channel-order fault from a wrong pin is to send a known
colour and look — and pure green is the one that distinguishes all three, since a
green/red channel swap is exactly what a GRB-versus-RGB mix-up produces. The override
restores itself on the battery thread's next tick.

### how values are encoded

Requirement 4.1 says fixed-width only, and requirement 5.3 asks for a scaling-factor RPC.
Taken together they suggest a split, and this design takes it:

- **The IMU stream carries the sensor's own `int16` register values, untouched.** The
  encoder is a `memcpy` out of the FIFO burst. That is what makes a DMA'd I²C read worth
  having: no per-sample arithmetic anywhere on the 208 Hz path. It also sidesteps a real
  range problem — gyro at ±2000 dps in centi-dps overflows `int16`, while raw LSBs never
  can — and halves the wire size against the CircuitPython port's `int32` fields.
- **Every other stream carries device-converted fixed point**, produced through Zephyr's
  ordinary sensor API. At ≤20 Hz there is no hot path to protect, and using the in-tree
  drivers is worth far more than saving a multiply — the SHT30 read is CRC-checked by its
  driver, and reimplementing that to obtain raw words would be a bad trade.

The scaling RPC then reports **every** stream uniformly, so the host has one decoder path
regardless of which side did the arithmetic. Each field is described as
`value_in_nano_SI = raw × num / den`, one entry per field **in wire order**, so the host
zips the scales against the sample's fields and needs no per-stream knowledge at all:

| stream | field | unit | num | den | i.e. |
|---|---|---|---|---|---|
| imu | gyro x,y,z | rad/s | 15271631 | 100 | 8.75 mdps/LSB at ±250 dps |
| imu | accel x,y,z | m/s² | 59820565 | 100 | 0.061 mg/LSB at ±2 g |
| magn | x,y,z | T | 100 | 1 | wire is deci-µT |
| env | temperature | °C | 10000000 | 1 | wire is centi-°C |
| env | humidity | %RH | 10000000 | 1 | wire is centi-% |
| env | light | — | 1000000000 | 1 | a raw ALS count |
| battery | millivolts | V | 1000000 | 1 | |
| battery | percent | % | 1000000000 | 1 | |
| battery | flags | — | 1000000000 | 1 | |
| button | code, pressed, pad | — | 1000000000 | 1 | |

The accel and gyro rows are ST's own sensitivities, from
`lsm6ds3tr-c_STdC/driver/lsm6ds3tr-c_reg.c:102` and `:127`. The gyro row is rounded to
the nearest 0.01 nrad/s, because the degree-to-radian factor is irrational and an exact
rational would be a fiction. Gyro is listed first because that is the order the chip's
FIFO produces, which is the order on the wire.

Three of those rows were wrong until the board corrected them.

**Dimensionless is `1000000000/1`, not `1/1`.** The whole table is in *nano*-SI, so
identity is a billion. A count of 1 written as `1/1` decodes to 1e-9. The symptom was a
light sensor that appeared dead: the chip reported a perfectly good clear-channel count of
129 and the host printed `0.0000`, having divided it by a billion. It is a mistake with no
error message and a plausible-looking result, which is the kind worth a named constant —
`kRawCount` in `src/rpc.cpp`.

**The light field is a raw count, not lux.** Zephyr's APDS9960 driver returns
`sample_crgb[0]` — the clear channel's raw ADC count — verbatim for `SENSOR_CHAN_LIGHT`
(`drivers/sensor/apds9960/apds9960.c:275`). It does no photometric conversion, so
reporting the field as lux would have been a unit this firmware cannot support.
Requirement 1.6 asks for "light level", which is what this is.

**The magnetometer's wire unit is deci-µT, not centi-µT.** This is the one that mattered.
The LIS3MDL's smallest full scale is ±4 gauss = ±400 µT, and centi-µT puts that at ±40000
— past an `int16` — so the *wire* clipped before the sensor did. That is not a theoretical
concern: the first board this ran on sat next to a magnet and railed all three axes at
once, reporting `(-32768, 32767, -32768)` — the *wire* clipping on top of a sensor that was
already saturated (see [the magnetometer](#the-magnetometer)). Deci-µT reaches ±3276.7 µT,
eight times the chip's range, and costs nothing real: the 0.1 µT step is below the part's
own ~0.32 µT RMS noise, and Earth's ~48 µT still gets about 480 counts. The saturating
clamp in `src/magn.cpp` is kept, now unreachable through this sensor, which is the state a
clamp should be in.

**The cost of this design, stated plainly:** a host can no longer decode a capture it did
not ask the scales for. Both CLIs therefore fetch the scale table at connect and print it,
and any future recorder must store it alongside the samples. The benefit is that changing
the accelerometer's full-scale range becomes a fact the host learns at runtime rather than
a constant it must be reflashed to agree with.

### wire format

One encoder, both transports. A batch is:

```
batch  = [ t_ms:u32 ][ seq:u16 ][ period_us:u16 ][ stream_id:u8 ][ count:u8 ]
         [ sample × count ]
```

Little-endian throughout; the header is 10 bytes, which leaves the `int16` sample array
2-byte aligned.

- `t_ms` is `k_uptime_get_32()` at the **first** sample in the batch, not at transmit. For
  the IMU it is derived by back-dating the drain time by `(count - 1) × period_us`, since
  the newest sample in the FIFO is the one that had just arrived. That inherits the drain's
  own jitter at batch boundaries — the measured `gap max` across a boundary is 5.9–6.7 ms
  against a 4.808 ms period — while the spacing *within* a batch is the chip's clock
  exactly. Deriving `t_ms` from a free-running sample counter instead would make the
  spacing look perfectly uniform and would be manufacturing that uniformity, so it is not
  done.
- `period_us` is the spacing between samples within the batch, from the chip's own ODR.
  The host back-dates the rest of the batch from it. This exists because the micro:bit
  host viewer has an `--accel-batch-time={spread,arrival}` flag whose `spread` mode has to
  guess that number from a nominal rate; sending it removes the guess.
- `seq` counts batches per stream and wraps at 16 bits. It is what lets the host tell a
  device-side drop (a gap in `seq`) from a link-side one (no gap, but a gap in `t_ms`).
- `count` is 1 for the unbatched streams.

Sample bodies:

| stream_id | stream | sample | bytes |
|---|---|---|---|
| 1 | imu | `int16 gx,gy,gz,ax,ay,az` | 12 |
| 2 | magn | `int16 x,y,z` (deci-µT) | 6 |
| 3 | env | `int16 temp_c_centi` · `uint16 rh_centi` · `uint16 light` | 6 |
| 4 | battery | `uint16 mv` · `uint8 percent` · `uint8 flags` | 4 |
| 5 | button | `uint16 code` · `uint8 pressed` · `uint8 _pad` | 4 |

Gyro precedes accel in the IMU sample because that is the order the LSM6DS3TR-C's FIFO
and its `OUTX_L_G`-onward register block produce them; reordering would mean touching
every sample for nothing. The two halves come out of one burst and therefore share one
sample instant exactly — the CircuitPython port made the same choice for the same reason,
and it is about *simultaneity*, not speed: read separately the two are about a millisecond
apart and independently timestamped, which is a lie about a single physical sample instant
that any downstream fusion inherits.

`period_us` is a `uint16`, so it tops out at 65.5 ms. That is fine for everything batched
here (4808 µs for the IMU, 50000 for the magnetometer) and meaningless for everything that
is not, where `count` is 1 and the field is set to 0 — there is nothing to back-date.

There is no CRC. GATT notifications are acknowledged, USB CDC is reliable, and COBS
resynchronises at the next delimiter; the CircuitPython port shipped without one and
recorded no framing errors it could attribute to corruption. The `seq` field is the thing
that would surface a problem, and it is cheap. Over USB it has read 0 gaps and 0 decode
errors in every run so far.

### streams

| id | stream | source | rate | batched |
|---|---|---|---|---|
| 1 | imu (accel + gyro) | the chip's FIFO watermark IRQ, on `P1.11` | 208 Hz | yes, exactly 10 |
| 2 | magn | DRDY trigger if routed, else timer — **currently the timer** | 20 Hz | yes, 2 |
| 3 | env (temperature, humidity, light) | timer | 1 Hz | no |
| 4 | battery | timer; emitted only on a ≥1 % change | ≤1 Hz | no |
| 5 | button | `input` callback | event | no |

208 Hz is the ODR the CircuitPython port settled on, and it is the one number here that
was chosen against a measurement rather than a datasheet. At rest, changing only the ODR:

| ODR | accel RMS σ | gyro RMS σ |
|---|---|---|
| 104 | 0.00968 m/s² | 0.00147 rad/s |
| **208** | 0.01021 m/s² | 0.00209 rad/s |
| ratio | **1.05×** | **1.42×** |

The gyro tracks the textbook √2-per-doubling almost exactly; the accel barely moves,
because its at-rest noise is dominated by ambient vibration rather than sensor bandwidth.
**Do not assume √2 applies to every stream — it did not here.** Going higher "for
headroom" buys another √2 of gyro noise for nothing.

The magnetometer batches **two** samples, not four. At 20 Hz the stream is 120 B/s and
batching buys throughput this link does not need; what it costs is latency, and the
design's budget is well under 100 ms. Two samples is exactly 100 ms and halves the
notification count; four would spend 200 ms to save nothing.

The IMU drains on the chip's own watermark interrupt, so a batch is
`CONFIG_LSM6DS3TRC_FIFO_WATERMARK_SAMPLES` (10) samples — about 48 ms — every time. The
drain still loops until the FIFO is empty, because one wake may cover several batches if a
lower-priority thread held the CPU, and because that same loop is what carries the timer
fallback if the trigger is ever unavailable. See [the imu's INT1
line](#the-imus-int1-line) for how the pin was found and what the interrupt measurably
bought over the 49 ms timer it replaced.

A backlog past 96 samples is treated as a stall: the FIFO is flushed rather than drained, because catching up would put stale samples on the
wire carrying plausible-looking back-dated timestamps, and a `seq` gap is the honest
report. That is the CircuitPython port's schedule-from-the-deadline rule applied to a
hardware queue.

### GATT layout

One custom 128-bit vendor primary service. Four notify characteristics, one per rate
class, so a host can subscribe to only what it needs; plus an RPC pair.

| Characteristic | Properties | Carries |
|---|---|---|
| imu | notify | stream 1 batches |
| magn | notify | stream 2 batches |
| env | notify | stream 3 batches |
| events | notify | streams 4 and 5 |
| rpc request | write | one request frame |
| rpc response | notify | one response frame |

`stream_id` stays in the header even though the characteristic already implies it, so that
the bytes on BLE and the bytes on USB are identical and there is exactly one encoder and
one decoder.

Batch size is computed at runtime from the negotiated MTU, the way
`microbit_v2_zephyr/src/ble.c` does it: `(bt_gatt_get_mtu(conn) - 3 - 10) / 12`. At
`CONFIG_BT_L2CAP_TX_MTU=247` that is 19 IMU samples per notification — about 11
notifications per second at 208 Hz, and about 91 ms of latency, which is the cap this
design budgets for. It is recomputed both in the CCC callback *and* in an
`att_mtu_updated` callback, because BlueZ often raises the MTU after the host has already
subscribed; with only the former, a subscription that arrived first would stay pinned at
19 usable bytes for the life of the connection. The USB path uses the same number, since
the CDC bulk endpoint is not the constraint on that link — one batch size, one encoder.

Measured: the board logs `ATT MTU 247 -> 19 IMU samples per notification` twice
per connection — once from the CCC callback and once from `att_mtu_updated` — and the
largest notification the host actually receives is 142 bytes, an 11-sample batch. The cap
is 19; what actually sets the batch size is the drain cadence, since a 48 ms drain has
about ten samples waiting. The MTU is the ceiling, not the schedule.

### restarting advertising

**Advertising must be restarted from the `recycled` callback, not from `disconnected`.**
This is the one thing in the BLE path that a build could not have caught and that a single
connection would not have caught either.

Calling `bt_le_adv_start()` from `disconnected` is the obvious thing to do and it fails
with `-ENOMEM`: the connection object is still held at that point, so a *connectable*
advertiser has no slot to take. The failure is quiet — one `LOG_ERR` line on a console
nobody is reading — and its symptom is not "BLE is broken" but "BLE worked once". The board
kept streaming perfectly over USB, kept answering its shell, and simply never advertised
again until it was rebooted. It took a second connection attempt to find.

Zephyr provides `recycled` for exactly this; its documentation calls it "the event to
listen for to start a new connection or connectable advertiser", and it fires once the
connection object has actually been freed. Three back-to-back connect/disconnect cycles now
succeed. Note that this needed no `k_work` and no extra thread — the callback is the
deferral.

Two calibrations from the CircuitPython port, which ran the same link from the same board:
it sustained roughly **110 notifications per second** on BlueZ at the **default 23-byte
MTU**, saturating around 100 Hz of IMU (~4.4 KB/s); and requesting the 7.5 ms minimum
connection interval measured *identical* to leaving the negotiated default alone, so that
code was deleted rather than kept as a plausible-looking no-op. The expected win here was
therefore MTU and the nRF52840's 2M PHY, not interval tuning.

That prediction held. This firmware carries 208 Hz of IMU plus everything else at about
30 notifications per second — well under the 110 that port managed — because each
notification carries eleven samples instead of one. It is doing roughly 2.7 KB/s of
samples, so the link is not the constraint and there is headroom left; the CircuitPython
figure stands as the floor it was quoted as.

### usb serial framing

The `cdc_acm_data` instance carries `cobs([channel] + payload) + 0x00`. BLE does not need
COBS because a GATT notification is already a delimited datagram; a byte stream is not, so
the serial path adds framing and the BLE path does not.

The leading channel byte is the part this document originally left out, and it is not
optional. Sample batches and RPC responses share the one pipe in the device-to-host
direction, so "distinguished by direction" only answers half the question — the host still
has to tell a batch from a reply. The channel byte is the serial analogue of a GATT
characteristic: on BLE the characteristic identifies the channel and nothing extra goes on
the wire; on serial one byte does the same job. `0x01` is a batch, `0x02` an RPC frame in
either direction, and `0x00` cannot be used because it is the delimiter. **The `payload`
bytes are identical on both transports**, which is what keeps there being one encoder and
one decoder.

Requirement 3.1's "combined packet to maximize throughput" is served by the same batching
the BLE path uses. The CircuitPython port measured its USB emit at 0.126 ms for a 20-byte
frame and found batching four frames saved ~1.6 % of loop time — i.e. **the transport was
never its bottleneck**, and it is even less of one here: at 208 Hz in 19-sample batches the
link carries about 11 frames per second and the measured host rate tracks the device rate
to within the window quantisation. Batching on this link is for the host's sake (fewer
wakeups, fewer syscalls per sample) rather than the board's.

### remote procedure calls

Same framing in both directions, on both transports: over BLE, a write to the rpc request
characteristic and a notification on the rpc response one; over USB, COBS frames on the
same `cdc_acm_data` pipe, distinguished from sample batches by direction.

```
request  = [ seq:u8 ][ opcode:u8 ][ args ]
response = [ seq:u8 ][ opcode:u8 ][ status:i8 ][ payload ]
```

`seq` is echoed so a host can match a reply to its request and time out on one that never
arrives. `status` is 0 on success and a negative errno otherwise.

| opcode | name | args | payload |
|---|---|---|---|
| `0x01` | get battery | — | `uint16 mv` · `uint8 percent` · `uint8 flags` |
| `0x02` | set stream | `uint8 stream_id` · `uint8 enable` | `uint8 stream_id` · `uint8 enable` as applied |
| `0x03` | get scale | `uint8 stream_id` | `uint8 stream_id` · `uint8 n` · n × (`uint8 unit` · `int32 num` · `int32 den`), one per sample field in wire order |
| `0x04` | get serial | — | 8 bytes, from `hwinfo_get_device_id()` |
| `0x05` | get build id | — | UTF-8, ≤ 48 bytes |

`0x02` returns the state it actually applied rather than echoing the request, so
"enable a stream this build does not have" is a visible no-op rather than a silent lie.
`0x04` uses the `hwinfo` API (`CONFIG_HWINFO=y`), which on this SoC reads the FICR
`DEVICEID` words — a real per-chip identifier, not a build constant. On the board here it
answers `2313EF1FD198023B`, which is the same eight bytes CircuitPython reported as its
board UID (`3B0298D11FEF1323`) in the opposite byte order — a useful confirmation that it
is the chip's identity and not something the firmware made up. `0x05` reports the
`VERSION` file's app version plus a `git describe` injected as a compile definition from
`CMakeLists.txt`, so a board can be traced back to a commit; it currently answers
`0.1.0+588f65e`.

The largest reply is `get scale` for the IMU: six fields, so 3 + 2 + 54 = 59 bytes. That
exceeds the 20 usable bytes of a default 23-byte ATT MTU, so a BLE host must negotiate a
larger one before the scale table is fetchable — which every modern stack does, and which
this design wants raised anyway.

### battery and the status led

Battery voltage comes from the existing `vbatt` node via `VOLTAGE_DIVIDER_DT_SPEC_GET` and
`voltage_divider_scale_dt()`, which applies the declared 100k/200k ratio rather than a
hand-written factor of two — **plus the `channel@5` node `app.overlay` has to add**,
without which every reading is 0 mV and nothing says so. The node is reached with
`DT_PATH(vbatt)` rather than `DT_NODELABEL`: the board dtsi declares it as a bare `vbatt
{ ... }` under the root with no label. Percent is the same crude linear 3.2 V–4.2 V curve the
CircuitPython port used — no lookup table, no OCV correction, no coulomb counting — and the
`flags` byte reports USB presence.

The LED is the NeoPixel on `P0.16`, driven by `worldsemi,ws2812-gpio`. The board's two
plain LEDs (red on `P1.09`, blue on `P1.10`) cannot make green and so cannot express three
bands; the NeoPixel is the only part on the board that can satisfy requirement 6. Bands and
hysteresis are ported directly:

| band | color | enter | leave |
|---|---|---|---|
| low | red | < 25 % | ≥ 28 % |
| medium | yellow | 25–60 % | < 22 % or ≥ 63 % |
| high | green | > 60 % | < 57 % |

The pixel is repainted **only on a band change**. `ws2812_gpio` bit-bangs the line with
inline assembly and interrupts locked for the duration of the transfer — roughly 30 µs for
one pixel at 24 bits — which is short, but it is also unnecessary a hundred times out of a
hundred and one. That rule lives inside `led::show()` rather than at its call site: the
battery thread calls it every cycle and `show()` returns early when the band it is handed
is already the one on the pixel, which is the one place that can actually know. It also
means an `fs led` override restores itself within a second instead of persisting until the
charge happens to cross a threshold.

Getting the pixel to display the *right* colour took a devicetree fix that has nothing to
do with colour — see [the status led's bit-bang
timing](#the-status-leds-bit-bang-timing).

`prj.conf` must carry `CONFIG_CLOCK_CONTROL_NRF=y` for this to compile at all. Without it
the nRF clock control resolves to the newer split `CLOCK_CONTROL_NRF_HFCLK`/`_LFCLK`
drivers, and `ws2812_gpio.c` then takes its non-`CONFIG_CLOCK_CONTROL_NRF` branch, which
has bit-rotted: `ws2812_gpio.c:139` references a `drv_data` that the surrounding function
no longer declares. It is a build error, not a runtime one, and the one-line Kconfig is
the whole fix. See [known limitations](#known-limitations-and-open-questions).

One warning inherited from the CircuitPython port, worth repeating because it cost a
working feature there:

> **Correction (2026-07-18).** This README previously stated that the pixel "reads green on
> USB regardless of the pack's true state", as a property of `read_battery()`. That was
> never measured, and the readings above contradict it. A feature was built on top of that
> claim before anyone checked it; the cost was an LED that displayed a constant amber.
> Measure the sensor before designing around its failure mode.

So: no charging-state correction, no capping the band while on USB, and no offset, until
somebody measures this board's divider on the charger. Charging does elevate terminal
voltage somewhat and a reading taken on the charger will run optimistic; an uncalibrated
fudge factor would be guesswork dressed as precision.

## building and flashing

```sh
export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -b adafruit_feather_nrf52840/nrf52840/sense/uf2 -p auto .
west flash
```

### the image must be linked at the code partition

**`CONFIG_USE_DT_CODE_PARTITION=y` in `prj.conf` is not optional, and its absence is
silent.** Neither of this board's `*_uf2_defconfig` files sets it, so the image links at
`CONFIG_FLASH_LOAD_OFFSET=0` and `zephyr.uf2` reports `start address: 0x0`. Writing that
through the UF2 bootloader lands the application on top of the s140 SoftDevice — and on
top of the **MBR at 0x0, which the Adafruit bootloader itself depends on**, taking the
bootloader with it and leaving SWD as the only way back. This board exposes SWD as pads
only.

It is an upstream board-support gap rather than a design question: the ItsyBitsy nRF52840
carries the same bootloader and the same flash layout and *does* set it
(`boards/adafruit/itsybitsy/adafruit_itsybitsy_nrf52840_defconfig`). With the line in
place the image links at the DTS `code_partition`, 0x26000.

**Check the build output, not `.config`.** `CONFIG_FLASH_LOAD_OFFSET` is written to
`.config` as `=0` when the setting is *missing* and disappears from the file entirely when
it is present — so grepping for it reads backwards. The line `west build` prints is the
reliable check:

```
Converted to uf2, output size: 472576, start address: 0x26000
```

`0x0` there means the setting was lost and the image must not be flashed.

Zephyr brings its own BLE controller and never calls the SoftDevice; it is left in place
only because the bootloader expects it to be there.

### getting into the bootloader

The `uf2` variant sets `CONFIG_BUILD_OUTPUT_UF2=y`, and `west flash` copies `zephyr.uf2`
to the bootloader's mass-storage drive — which requires the board to be *in* the
bootloader, in UF2 mode. Three ways, in descending order of convenience:

- **`fs bootloader` at the shell.** This firmware writes the Adafruit bootloader's
  `DFU_MAGIC_UF2_RESET` (`0x57`) to `NRF_POWER->GPREGRET` and resets. GPREGRET survives a
  soft reset, so the bootloader comes up in UF2 mode and `FTHRSNSBOOT` appears. This is the
  reason that command exists: without it every reflash costs a hand on the board, and the
  board is normally somewhere a hand is not.
- **Double-tap the reset button**, which is the only option when the running firmware is
  something else, or is wedged.
- **Serial DFU**, which is where a 1200-baud touch on a *CircuitPython* board lands (it
  enumerates CDC only, with no mass-storage drive, so `west flash` cannot see it). Flash it
  with `adafruit-nrfutil` instead — this is how the very first Zephyr image got onto a board
  that was running CircuitPython:

  ```sh
  adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application build/zephyr/zephyr.hex fw.zip
  adafruit-nrfutil dfu serial --package fw.zip -p /dev/ttyACM0 -b 115200 --singlebank
  ```

  Zephyr's own CDC ACM implements no 1200-baud hook, so the touch only works against
  firmware that does.

The host sees **two** ACM devices once the application is running, told apart by their
interface string descriptors rather than by enumeration order. `west build -t menuconfig`
for the usual reasons.

## console shell

The shell runs on `board_cdc_acm_uart` — a USB CDC device, so no extra wiring. Resolve
which port it is by descriptor rather than assuming:

```sh
grep -l "Feather Sense console" /sys/class/tty/ttyACM*/device/interface
pyserial-miniterm /dev/ttyACM0 115200      # ships in west's venv; Ctrl-] to quit
```

The baud rate is ignored — this is a USB CDC endpoint, not a real UART — but every tool
wants one, so 115200 is as good as any.

Beyond the built-ins, the application registers an `fs` command group, guarded by
`#ifdef CONFIG_SHELL`, following both existing Zephyr areas:

| Command | Answers |
|---|---|
| `fs stats` | batches emitted, drops per transport, USB frame and rx-error counters, the current IMU batch size, and which streams are enabled |
| `fs imu` | `WHO_AM_I` and which part it means, samples and batches so far, FIFO overruns and stall flushes |
| `fs battery` | the last reading — millivolts, percent, and whether USB is present |
| `fs env` | what the last SHT30 fetch cost, in microseconds, and how many have failed |
| `fs stream <id> <0\|1>` | enable or disable one stream, the same thing RPC opcode `0x02` does |
| `fs led <r> <g> <b>` | drive the pixel to a known colour; restores itself on the battery thread's next tick |
| `fs bootloader` | reboot into the UF2 bootloader, so a reflash needs no hand on the board |

`CONFIG_I2C_SHELL` earns its place here more than anywhere else in the repo:

```
i2c scan i2c@40003000                 # which parts this board revision actually has
i2c read_byte i2c@40003000 0x6a 0x0f  # WHO_AM_I: 0x69 = LSM6DS33, 0x6a = LSM6DS3TR-C
```

The device argument is the devicetree node's full name, not its label — `i2c0` is a label
and the shell will not accept it. Tab completion over the device list is the reliable way
to get it right. That second command is what settled the IMU question: **this board
answers `0x69`, an LSM6DS33.**

`CONFIG_SENSOR_SHELL` and `CONFIG_ADC_SHELL` give a way to read every stream without a
host program attached at all, which is what tells a driver fault from an application one —
`sensor get lsm6ds3trc@6a` reading 9.97 m/s² on Z while the stream reported zeros would
have located a bug in one call. `CONFIG_INPUT_SHELL` does the same for the button:
`input report 1 11 1` injects a synthetic press and drives the whole path, notification
included, because `buttons.cpp` registers with `INPUT_CALLBACK_DEFINE(NULL, ...)` and
cannot tell a synthetic event from a real one.

## tests

A `native_sim` ztest suite under `tests/`, built the way
`rpi_pico_zephyr_debug/tests/temp_convert/` is: the pure logic lives in translation units
free of Zephyr headers, and the *same* `.cpp` files are compiled into both the firmware and
the host test.

```sh
west build -b native_sim -p auto -d build_test tests/codec && ./build_test/zephyr/zephyr.exe
west twister -p native_sim -T tests        # 52 test cases, 4 configurations
```

Two modules qualify, and only two — the rest of this firmware is hardware:

- `src/codec.cpp` — batch header pack/unpack, the scale-field layout, and COBS
  encode/decode. The COBS tests round-trip every length up to a full batch in both the
  all-non-zero and all-zero shapes, and pin the 253/254/255-byte block-split boundaries
  explicitly; that is where every COBS implementation goes wrong. The header and
  scale-field layouts are asserted against literal expected bytes rather than through this
  file's own unpacker — a test that only round-trips would agree with any layout — and that
  is what caught a hand-converted hex constant during development.
- `src/battery_level.cpp` — divider millivolts to percent, and the hysteresis band
  function. The band function is pure and has a genuinely interesting property to test:
  that it moves in both directions and that no sequence of inputs can latch it. A sweep
  from 100 % down to 0 and back is what catches an implementation that widened one
  threshold without narrowing its opposite — which passes every individual crossing test
  and still gets stuck, and is the defect the CircuitPython port's README records shipping
  as an LED that displayed a constant amber.

## host side

Four programs in one nested pixi environment at `host/`, separate for the same reason
`microbit_v2_zephyr/host/` is: neither bleak nor rerun is a repo-wide dependency. They come
in a pair per transport plus a viewer, and share definitions through a plain import rather
than a second copy.

| Program | Transport | Does |
|---|---|---|
| `feather_protocol.py` | neither — no I/O | the wire format, the decoders, the scale table, the RPC frames, and the rate arithmetic |
| `read_serial.py` | USB CDC, pyserial | owns the serial link; measures it |
| `read_ble.py` | BLE, bleak | owns the BLE link; measures it |
| `feather_rerun.py` | either, by flag | plots what the other two decode |

`feather_protocol.py` is the **only** host-side definition of the wire format. Every
constant in it carries a `# Must match ../src/<file>: <SYMBOL>` comment naming the firmware
symbol it mirrors, as `microbit_v2_zephyr/host/ble_stream.py` does. It contains no I/O: the
two readers own their transports, and both hand bytes to the same decoder. `feather_rerun.py`
imports `SerialLink` and `BleLink` from the readers rather than opening links of its own,
the way `ble_rerun.py` imports `ble_stream`.

`read_serial.py` resolves the data port by its **USB interface string descriptor** and
refuses to guess: `find_port()` reads `/sys/class/tty/ttyACM*/device/interface` and matches
`"Feather Sense data"`, erroring out if nothing or more than one thing claims it.

```sh
cd host
pixi run serial --seconds 20
pixi run ble --seconds 20
pixi run viz --transport ble --window 10
pixi run type
```

### measuring the link

Both readers report per stream, once a second, and the reporting rule is inherited whole
from the CircuitPython port because getting it wrong there over-reported by ~10 %:

- **`dev`** — `(count - 1) × 1000 / (last_ts - first_ts)`, from device timestamps. This is
  the number to quote. It needs no clock synchronisation between board and host, and
  `count - 1` is the number of intervals actually spanned. A consequence worth expecting:
  a stream that delivers only one sample in a window reports `0.00`, because one sample
  spans no intervals. That is the rule being honest, not a bug — the 1 Hz env stream reads
  `dev 0.00` in a 1 s window and a true rate over a longer one.
- **`host`** — `count / measured elapsed`. It only tells you the link kept up. Divide by
  *measured* elapsed, never by the nominal window: a window gated on `>= 1.0 s` is always
  overshot, and a true 91 Hz stream then reports "100/s", flatteringly.
- **`gap max`** — the largest device-timestamp interval seen. A rate on target with an
  outsized gap means the stream stalled and caught up in a burst, which neither average
  shows.
- **`errors`** — frames the decoder rejected. It should read 0. Treat anything else as
  contamination of the data channel, not as noise to tune out.
- **`seq gaps`** — new here, and the reason `seq` is in the header: it separates a
  device-side drop from a link-side one, which on the CircuitPython port could only be
  inferred from the shape of the timestamp spacing.

Each reader fetches the scale table over RPC at connect and prints it before streaming,
since decoding depends on it.

What that produces on this board, over USB, with everything running:

```
data port /dev/ttyACM1
serial    2313EF1FD198023B
build     0.1.0+588f65e
...
[  4.0s] errors 0
  imu      dev  208.15/s  host  203.28/s  batches    20  gap max     6.7 ms  seq gaps 0
  magn     dev   20.00/s  host   19.93/s  batches    10  gap max    52.0 ms  seq gaps 0
  env      dev    0.00/s  host    1.00/s  batches     1  gap max     0.0 ms  seq gaps 0
```

And over BLE, which is the same numbers through a different pipe:

```
[ 19.1s] errors 0  largest notification 142 B (ATT MTU >= 145)
  imu      dev  208.97/s  host  214.12/s  batches    21  gap max     5.7 ms  seq gaps 0
  magn     dev   20.00/s  host   19.92/s  batches    10  gap max    50.0 ms  seq gaps 0
  env      dev    0.00/s  host    1.00/s  batches     1  gap max     0.0 ms  seq gaps 0
  battery  dev    0.00/s  host    1.00/s  batches     1  gap max     0.0 ms  seq gaps 0
```

`largest notification` is there instead of an MTU because **bleak cannot report the
negotiated ATT MTU on BlueZ**: `BleakClient.mtu_size` warns that it is returning the
default and then returns 23 forever. Printing that would be worse than printing nothing —
it claims the link is at the minimum when the board's own log says 247. The largest
notification actually received is a measurement of the same quantity and a lower bound on
it, and the two can be cross-checked against the console.

`dev 208.15` against a chip ODR of 208 is the headline: the sample spacing is the chip's
own clock, not the host's. The magnetometer's 52 ms `gap max` is its 50 ms sample spacing
plus jitter, as expected for two samples per batch at 20 Hz. The IMU's 6.7 ms is the drain
jitter at batch boundaries described under [wire format](#wire-format); within a batch the
spacing is exactly `period_us`. `host` oscillating between 203 and 214 is window
quantisation — 20 versus 21 batches — and is why `dev` is the number to quote.

### visualizing the streams

`feather_rerun.py` follows `microbit_v2_zephyr/host/ble_rerun.py`: `rr.init` then
`rr.spawn(memory_limit=...)` (not `rr.init(spawn=True)`, whose bool form cannot forward the
limit), an `rrb.Grid` of `rrb.TimeSeriesView`s built by a `build_blueprint`, static
`rr.SeriesLines` styling logged once, and `rr.set_time` + `rr.log(..., rr.Scalars(...))`
per sample. Six views: acceleration, angular rate, magnetic field, environment, battery,
and button state.

**Unit conversion happens here, host-side, from the scale table the device reported.**
There is no hard-coded conversion factor in the viewer — it asks the board what a raw count
means and multiplies. The one cosmetic exception is the magnetometer, plotted in µT rather
than the tesla the scale table reports, because nobody reads a magnetometer plot in tesla.

Batched samples are back-dated using the batch's own `period_us`, so there is no
`--accel-batch-time` flag to choose and no nominal rate to assume. Everything is plotted
against the *device* timeline, rebased so the session starts at zero, which is the same
timeline the rate numbers are computed on.

## divergences from the circuitpython port

`~/code/remapy/adafruit_feather_sense/` runs on this same board. What changed, and why:

| | CircuitPython port | here | why |
|---|---|---|---|
| Framing | COBS on both transports | COBS + a channel byte on serial only | A GATT notification is already a datagram, and the characteristic already says which channel |
| Payload | one sensor sample per frame, `int32` pre-scaled | batches of raw `int16` | Requirement 3.2; and pre-scaling was pure cost on a 208 Hz path |
| Scales | a shared `SCALES` table in a file both sides import | reported over RPC | Requirement 5.3, and a range change stops needing a reflash |
| Timestamps | per-sample, host-derived rate | per-batch, plus `period_us` | Removes the viewer's spread-vs-arrival guess |
| Drop detection | inferred from timestamp spacing | explicit `seq` per stream | Separates device drops from link drops |
| Data channel | shared with the console | its own CDC ACM instance, named by its USB interface descriptor | The status-bar contamination bug cannot occur, and neither port has to be identified by enumeration order |
| Sampling | cooperative polling loop | FIFO watermark + preemptive threads | CircuitPython has no user-defined interrupt handlers; this is the port's single biggest unlock |
| Environmental | removed entirely (blocked ~152 ms/s) | kept, on the lowest-priority thread | Requirements 1.4–1.6 ask for them — and Zephyr's driver runs the SHT30 in periodic mode, so the read this port deleted over is not the read Zephyr does |
| Derived motion | gravity / linear accel computed on the host | not computed at all | Out of scope; belongs to the future `dfg` graph |
| RPC | none — everything was a reflash | five opcodes | Requirement 5 |
| Status LED | NeoPixel, bands + hysteresis | same, unchanged | It was already right |

One class of lesson does **not** carry over. The CircuitPython port's whole cost table —
`encode` at 1.275 ms per frame, COBS at 0.708 ms, the +33 % from fusing a `struct.pack` —
is an interpreter artifact. In C++ those steps cost microseconds. What survives the
language change is narrower and more valuable: the burst-read simultaneity argument, the
ODR-versus-noise trade, the schedule-from-the-deadline rule (`due += interval`, never
`due = now + interval`, plus a stall clamp that drops a backlog rather than bursting stale
samples), and the measurement discipline in [measuring the link](#measuring-the-link).

## known limitations and open questions

Split three ways: what a real board settled, what is a live limitation, and what remains
unverified. Keeping those apart is this document's main job — the CircuitPython port's
README carries a correction recording what designing on an unmeasured claim cost it, and
three of the entries below were promoted from the third list to the first by ten minutes
with a shell.

### settled by running it

- **The IMU is an LSM6DS33.** `i2c read_byte i2c@40003000 0x6a 0x0f` answers `0x69`.
  Adafruit swapped the DS33 for the pin-compatible LSM6DS3TR-C in January 2024; this unit
  predates that, or the swap did not reach it. The out-of-tree driver accepts both ids and
  drives this one through ST's LSM6DS3TR-C register driver without complaint, which is the
  outcome that design anticipated. `sensor get lsm6ds3trc@6a` reads 9.97 m/s² on Z with the
  board flat.
- **The multi-sample FIFO burst read works.** The driver reads up to 20 records — 240
  bytes — in one I²C burst from `FIFO_DATA_OUT_L`, relying on the chip re-presenting that
  two-register window rather than running off the end of the register map. It does: the
  measured device-timestamp rate is 208.4/s against a 208 Hz ODR, and zero FIFO overruns or
  misalignments have been logged.
- **`CONFIG_I2C_NRFX_TWIM=y` is what the build resolves to**, and TWIM is what is bound —
  but it cannot be *asserted* in `prj.conf`, which was the third promptless-symbol trap
  after `USE_STDC_LSM6DS3TR_C` (and, unlike that one, `I2C_NRFX_TWIM` is set purely by the
  overlay). `grep CONFIG_I2C_NRFX_TWIM build/zephyr/.config` is the check.
- **The IMU's INT1 is routed, to `P1.11`, and the FIFO is drained on it.** Nothing
  documented that pin — this document had it as an open question and proposed a check that
  would not have worked. Driving INT1 statically high instead, and reading every free GPIO
  with the `devmem` shell, found it in one pass; INT2 is not routed. The trigger path in
  `drivers/lsm6ds3trc/lsm6ds3trc_trigger.c` is now compiled in and running, and what it
  bought is measured rather than assumed: every batch is exactly the watermark, where the
  49 ms timer it replaced delivered an eleventh sample 21.7 % of the time. See [the imu's
  INT1 line](#the-imus-int1-line).

- **The LIS3MDL and APDS9960 fallbacks resolve on their own.** With no `irq-gpios` and no
  `int-gpios`, Kconfig picks `CONFIG_LIS3MDL_TRIGGER_NONE=y` and
  `CONFIG_APDS9960_FETCH_MODE_POLL=y` with no help, and both sensors read correctly that
  way.
- **The SHT30 read is cheap, and the ~152 ms figure was never applicable.** See [the
  environmental read](#the-environmental-read). The driver defaults to periodic mode, so
  there is no conversion to block on; what the assumption hid was a fetch-vs-produce race,
  fixed with `CONFIG_SHT3XD_MPS_2=y`.
- **The magnetometer reads Earth's field, and the sensor was never at fault.** 48.3 µT
  total once the board was moved away from a magnet. See [the
  magnetometer](#the-magnetometer) for how a saturated reading was told apart from a broken
  part, which took one register write.

- **The NeoPixel works, and it is on `P0.16`.** It shows green at a `high` battery band and
  responds correctly to `fs led`, so the pin, the GRB colour mapping and the driver path
  are all confirmed. Getting there needed a devicetree fix unrelated to any of them — see
  [the status led's bit-bang timing](#the-status-leds-bit-bang-timing). What has *not* been
  seen is the band actually *changing*, which needs a real discharge.

- **Stack sizes are measured, not guessed.** `CONFIG_INIT_STACKS=y` and
  `CONFIG_THREAD_NAME=y` are set so `kernel thread stacks` reports high-water marks, and it
  does: imu 744/2048 (36 %), magn 496/1536 (32 %), env 400/1536 (26 %), battery 392/1024
  (38 %), tx_ble 520/2048, tx_usb 448/2048, usb_rx 568/2048, and the input subsystem's own
  thread 392/1024 (38 %) *after* a button event has pushed a batch through
  `streams::emit()`. That last one is why `CONFIG_INPUT_THREAD_STACK_SIZE` is left at its
  default: `emit()` puts a 242-byte batch on the caller's stack, which looked worth a bump
  until the number was read.

- **A drop is visible to the host as a gap in `seq`, which is what that field is for.**
  Never mind that both counters read 0 in normal running — the claim is only worth
  anything if it fires when it should, and it does. Stalling the serial reader for 6 s
  without draining the port: the device counted **104 dropped USB frames**, the host
  counted **69 IMU and 31 magnetometer sequence gaps** (which adds up, allowing for the
  env and battery batches in the same window), its `gap max` jumped to 3.4 s, and its
  **decode errors stayed at 0** — frames were dropped whole, never truncated, which is what
  the all-or-nothing ring-buffer write in `usb::send()` is there to guarantee. The stream
  resumed clean.

  The drops landed in the *frame* counter, not the transmit-queue one. `usb::send()` never
  blocks, so the queue drains as fast as it fills and `queue full usb` stays 0 even under
  heavy backpressure; the real drop is the CDC ring filling. `fs stats` labels the two
  distinctly for that reason — they sit next to each other and would otherwise read as
  contradicting one another.

- **Both transports stream at once.** USB and BLE simultaneously, each at the full rate,
  with 0 drops on either side and 0 sequence gaps. The two transmit queues are independent
  and neither starves the other.

- **BLE carries the full stream, and reconnects.** All four notify characteristics, the
  RPC pair, MTU negotiation to 247, and three consecutive connect/disconnect cycles: 208.4
  samples/s from device timestamps, 0 decode errors, 0 sequence gaps and 0 device-side
  notification drops over 20 s. The rerun viewer runs over it too. The one defect found was
  the re-advertise path — see [restarting advertising](#restarting-advertising) — and it
  is exactly the shape the earlier version of this list predicted: "something small in that
  untested path, not a throughput surprise".

- **`ws2812_gpio.c` does not compile without `CONFIG_CLOCK_CONTROL_NRF=y`.** Confirmed
  again by this build. The symbol is now also deprecated in 4.4.99 and warns about it, so
  this will need revisiting; the alternatives are `worldsemi,ws2812-spi` off `spi1`, at the
  cost of a byte of buffer per bit and one SPI pin, or the two-word upstream fix to the
  driver's `#else` branch at `ws2812_gpio.c:139`.

### live limitations

- **The battery path has not been exercised with a real pack.** The ADC channel is
  configured and the divider ratio comes from devicetree, but every reading so far was
  taken with no cell attached. Percent, the band hysteresis and the `flags` USB bit are
  covered by host tests and by construction, not by a discharge curve.
- **The battery's "≥1 % change" rule throttles nothing in practice.** Requirement 1.7 is
  implemented literally and the percent is an integer, so any change is at least one point
  — but the ADC reading dithers by about 10 mV, which *is* one point on the 3.2–4.2 V
  linear curve, so the stream emits roughly every second rather than rarely. It is 4 bytes
  a second and harmless, and the requirement is met as written, but it is not doing the job
  it was put there to do. The fix is to filter the millivolts before converting — the
  SAADC's own `zephyr,oversampling` property is the cheapest version, and a longer average
  is the better one. Neither has been done, because the right time constant should be
  chosen against a discharge curve and nobody has measured one; an invented one would be
  the same mistake this document keeps warning about, in a smaller costume.

- **`period_us` is a `uint16`**, so a batched stream slower than about 15 Hz cannot express
  its spacing. Nothing batched here is (the magnetometer is 50 000 µs), and the unbatched
  streams set it to 0, but a future 5 Hz batched stream would need a wider field or a
  different unit.

### still unverified

- **Whether the LIS3MDL's DRDY and the APDS9960's INT are routed.** Undocumented in the same
  way INT1 was, and now answerable in the same way — see [the imu's INT1
  line](#the-imus-int1-line) for the method. Both fallbacks are automatic and confirmed
  working, so this is a performance question rather than a correctness one, and neither
  sensor is fast enough for the answer to change much: the other 26 candidate pins were all
  quiet during that sweep, so whichever they are, nothing was driving them.
- **Nothing has run for longer than a minute.** The `seq` wrap at 16 bits, the `t_ms` wrap
  at 32 bits (49.7 days), queue behaviour under a host that stops reading, and the stall
  clamp in `src/imu.cpp` (which needs a 96-sample backlog to fire) are all untested by
  elapsed time.
- **Pressure and the microphone are deliberately out of scope.** The BMP280 answers at
  `0x77` on the bus scan and Zephyr's `bosch,bme280` driver accepts its chip id `0x58` at
  `drivers/sensor/bosch/bme280/bme280.c:358`; the nRF52840's PDM peripheral has a driver and
  EasyDMA. Neither is in the requirements. If pressure is added later: put the BMP280 in
  normal mode so a read is a register fetch rather than a forced conversion, lower the
  pressure oversampling from its default, and derive altitude on the host rather than paying
  a second conversion for it.
- **The `dfg` integration named as future work needs no firmware change.** `dfg` has no
  concept of a free-running source node — a node with zero inputs is a validation error,
  and graph inputs are the only sources. A host reader is therefore an *application driving
  the graph*, calling `graph.inject(...)` then `run_until_idle()`, not a node inside it.
  The only thing this design owes that future is what it already provides: an integer
  sample timestamp per sample, which `dfg`'s nanosecond `Message.timestamp` wants
  multiplied by 1e6.
