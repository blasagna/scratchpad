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
decisions below. Where this document quotes a number, that is where it came from.

## status

**No firmware has been written, and nothing has been run on the board.** This document is
the design contract; the firmware and the host programs land afterwards, in the order
`dfg/` used — contract first, port second.

What *has* been done is that the [devicetree overlay](#devicetree-overlay) below was
built, minus the IMU node whose driver does not exist yet. `west build -b
adafruit_feather_nrf52840/nrf52840/sense/uf2` links and emits a `zephyr.uf2` (200912 B
flash, 43996 B RAM for a config with I²C, the three in-tree sensors, ADC, the LED strip,
input, shell and Bluetooth). That build is what turned up the two Kconfig traps recorded
below, so the overlay and the `prj.conf` notes are verified rather than merely plausible.
Every other Zephyr path, symbol, binding and driver behaviour named here was checked
against the 4.4.99 tree in `~/zephyrproject`.

Everything about how the *hardware* behaves is either quoted from the CircuitPython port
or listed in [known limitations and open questions](#known-limitations-and-open-questions)
as unverified. The two are kept apart on purpose.

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
| Accelerometer + gyroscope | LSM6DS33 **or** LSM6DS3TR-C | `0x6a` | **none in tree** | Needs an out-of-tree driver — see below |
| Magnetometer | LIS3MDL | `0x1c` | `st,lis3mdl-magn` | Driver exists; needs an overlay node |
| Temperature + humidity | SHT30 | `0x44` | `sensirion,sht3xd` | **Already in the board DTS** (`sht3xd@44`) |
| Light level | APDS9960 | `0x39` | `avago,apds9960` | Driver exists; needs an overlay node |
| Battery | divider to `AIN5`, 100k/200k | — | `voltage-divider` | **Already in the board DTS** (`vbatt`) |
| User button | switch on `P1.02`, active low | — | `gpio-keys`, `INPUT_KEY_0`, alias `sw0` | **Already in the board DTS** |
| Status LED | NeoPixel on `P0.16` | — | `worldsemi,ws2812-gpio` | Driver exists; needs an overlay node |
| Pressure | BMP280 | `0x77` | `bosch,bme280` (accepts chip id `0x58`) | Available, **not used** |
| Microphone | PDM MEMS | — | `nordic,nrf-pdm` | Available, **not used** |

Four consequences worth stating outright.

**Zephyr has no driver for this board's IMU.** `drivers/sensor/st/` ships `lsm6ds0`,
`lsm6dsl`, `lsm6dso`, `lsm6dso16is` and the `lsm6dsv*` family. The LSM6DS33 and the
LSM6DS3TR-C are in none of them. The nearest fit is `st,lsm6dsl`, whose `WHO_AM_I` check
expects `0x6a` — which the LSM6DS3TR-C happens to answer, and the LSM6DS33 (`0x69`) does
not — but that driver offers data-ready triggers only, with **no FIFO support at all**, so
it cannot serve requirement 3.2's batching from a hardware-clocked source. What *is*
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

**The console is already on USB, and that is a problem the board solves for free.** The
`sense/uf2` board variant includes `boards/common/usb/cdc_acm_serial.dtsi`, which creates
a `board_cdc_acm_uart` CDC ACM instance and chooses it for `zephyr,console` and
`zephyr,shell-uart`. The CircuitPython port's single most annoying defect was that its
console *was* its data channel, so the runtime's status-bar escape sequence landed
between two binary frames on every host attach. Here the overlay declares a **second**
CDC ACM instance for data, and log output can never reach it. That is a structural fix,
not a workaround.

## design

### devicetree overlay

```dts
#include <zephyr/dt-bindings/i2c/i2c.h>
#include <zephyr/dt-bindings/led/led.h>

/*
 * The board DTS declares only the SHT30. Everything else on the internal I²C bus,
 * plus the NeoPixel, is added here.
 */
&i2c0 {
	/* EasyDMA. The board files use "nordic,nrf-twi", which is the non-DMA
	 * peripheral; TWIM is the same pins with DMA behind them. */
	compatible = "nordic,nrf-twim";
	clock-frequency = <I2C_BITRATE_FAST>;   /* 400 kHz */

	imu: lsm6ds3trc@6a {
		compatible = "scratchpad,lsm6ds3trc";   /* out-of-tree, see drivers/ */
		reg = <0x6a>;
		/* irq-gpios: only if INT1 is routed. See known limitations. */
	};

	magn: lis3mdl@1c {
		compatible = "st,lis3mdl-magn";
		reg = <0x1c>;
		/* irq-gpios: only if DRDY is routed. See known limitations. */
	};

	light: apds9960@39 {
		compatible = "avago,apds9960";
		reg = <0x39>;
		/* int-gpios: only if INT is routed. Absent selects
		 * CONFIG_APDS9960_FETCH_MODE_POLL automatically. */
	};
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
 * `board_cdc_acm_uart` keeps the console, the shell and the log, so the two
 * never share a pipe.
 */
&zephyr_udc0 {
	cdc_acm_data: cdc_acm_data {
		compatible = "zephyr,cdc-acm-uart";
	};
};
```

The battery divider and the user button need no overlay at all: the common board dtsi
already declares a `voltage-divider` node on `&adc 5` with `output-ohms = <100000>` and
`full-ohms = <200000>`, and a `gpio-keys` `button0` on `gpio1 2` with
`zephyr,code = <INPUT_KEY_0>` and the `sw0` alias. Battery reads go through
`<zephyr/drivers/adc/voltage_divider.h>`; the button goes through the `input` subsystem,
which means no debounce code and no polling loop of our own.

### threads and data flow

| Thread | Prio | Woken by | Rate | Does |
|---|---|---|---|---|
| imu | 5 | FIFO watermark IRQ, else `k_timer` | 208 Hz | drain N records → one batch → both tx queues |
| magn | 6 | DRDY trigger, else `k_timer` | 20 Hz | fetch → convert → batch |
| tx ble | 7 | `k_msgq` | per connection interval | `bt_gatt_notify` per stream |
| tx usb | 7 | `k_msgq` | as drained | COBS-encode → write to `cdc_acm_data` |
| env | 8 | `k_timer` | 1 Hz | SHT30 + APDS9960 fetch |
| battery | 9 | `k_timer` | 1 Hz | ADC read; emit only on a ≥1 % change; repaint the LED on a band change |

Button events arrive on an `input` subsystem callback with no thread of their own, as in
the micro:bit application, and are pushed straight onto both tx queues.

Two notes on the priorities. The env thread is the lowest of the sampling threads
deliberately: Zephyr's `sht3xd` driver does a blocking single-shot conversion, and the
CircuitPython port's most expensive lesson was that 1 Hz environmental reads stalled its
loop for **~152 ms of every second** and cost ~7.6 IMU samples per second. Preemptive
scheduling *contains* that here — a blocked env thread cannot delay a higher-priority IMU
thread — but it does not make the read cheaper, and the actual cost is unmeasured until
this runs. The tx threads sit above env and battery so a full queue drains ahead of new
low-rate work.

Everything is threads and message queues; there is no `k_work` anywhere, matching both
existing Zephyr areas.

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
`value_in_nano_SI = raw × num / den`:

| stream | field | unit | num | den | i.e. |
|---|---|---|---|---|---|
| imu | accel x,y,z | m/s² | 59820565 | 100 | 0.061 mg/LSB at ±2 g |
| imu | gyro x,y,z | rad/s | 15271631 | 100 | 8.75 mdps/LSB at ±250 dps |
| magn | x,y,z | T | 10 | 1 | wire is centi-µT |
| env | temperature | °C | 10000000 | 1 | wire is centi-°C |
| env | humidity | %RH | 10000000 | 1 | wire is centi-% |
| env | light | lux | 1000000000 | 1 | wire is lux |
| battery | millivolts | V | 1000000 | 1 | |
| battery | percent | % | 1000000000 | 1 | |

The accel and gyro rows are ST's own sensitivities, from
`lsm6ds3tr-c_STdC/driver/lsm6ds3tr-c_reg.c:102` and `:127`. The gyro row is rounded to
the nearest 0.01 nrad/s, because the degree-to-radian factor is irrational and an exact
rational would be a fiction.

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

- `t_ms` is `k_uptime_get_32()` at the **first** sample in the batch, not at transmit.
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
| 2 | magn | `int16 x,y,z` | 6 |
| 3 | env | `int16 temp_c_centi` · `uint16 rh_centi` · `uint16 lux` | 6 |
| 4 | battery | `uint16 mv` · `uint8 percent` · `uint8 flags` | 4 |
| 5 | button | `uint16 code` · `uint8 pressed` · `uint8 _pad` | 4 |

Gyro precedes accel in the IMU sample because that is the order the LSM6DS3TR-C's FIFO
and its `OUTX_L_G`-onward register block produce them; reordering would mean touching
every sample for nothing. The two halves come out of one burst and therefore share one
sample instant exactly — the CircuitPython port made the same choice for the same reason,
and it is about *simultaneity*, not speed: read separately the two are about a millisecond
apart and independently timestamped, which is a lie about a single physical sample instant
that any downstream fusion inherits.

There is no CRC. GATT notifications are acknowledged, USB CDC is reliable, and COBS
resynchronises at the next delimiter; the CircuitPython port shipped without one and
recorded no framing errors it could attribute to corruption. The `seq` field is the thing
that would surface a problem, and it is cheap.

### streams

| id | stream | source | rate | batched |
|---|---|---|---|---|
| 1 | imu (accel + gyro) | FIFO watermark | 208 Hz | yes |
| 2 | magn | DRDY trigger, else timer | 20 Hz | yes |
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

Batch size is computed at runtime from the negotiated MTU inside the CCC callback, the way
`microbit_v2_zephyr/src/ble.c` does it: `(bt_gatt_get_mtu(conn) - 3 - 10) / 12`. At
`CONFIG_BT_L2CAP_TX_MTU=247` that is 19 IMU samples per notification — about 11
notifications per second at 208 Hz. It is capped so latency stays bounded well under
100 ms.

Two calibrations from the CircuitPython port, which ran the same link from the same board:
it sustained roughly **110 notifications per second** on BlueZ at the **default 23-byte
MTU**, saturating around 100 Hz of IMU (~4.4 KB/s); and requesting the 7.5 ms minimum
connection interval measured *identical* to leaving the negotiated default alone, so that
code was deleted rather than kept as a plausible-looking no-op. The expected win here is
therefore MTU and the nRF52840's 2M PHY, not interval tuning — and if raising the MTU does
not move the number, the next thing to check is the notification rate, not the interval.

### usb serial framing

The `cdc_acm_data` instance carries `cobs(batch) + 0x00`. BLE does not need COBS because a
GATT notification is already a delimited datagram; a byte stream is not, so the serial path
adds framing and the BLE path does not. Both carry the same `batch` bytes inside.

Requirement 3.1's "combined packet to maximize throughput" is served by the same batching
the BLE path uses, with the batch sized to the CDC bulk endpoint rather than to an ATT MTU.
The CircuitPython port measured its USB emit at 0.126 ms for a 20-byte frame and found
batching four frames saved ~1.6 % of loop time — i.e. **the transport was never its
bottleneck**, and it will be even less of one here. Batching on this link is for the
host's sake (fewer wakeups, fewer syscalls per sample) rather than the board's.

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
| `0x03` | get scale | `uint8 stream_id` | `uint8 stream_id` · `uint8 n` · n × (`uint8 unit` · `int32 num` · `int32 den`) |
| `0x04` | get serial | — | 8 bytes, from `hwinfo_get_device_id()` |
| `0x05` | get build id | — | UTF-8, ≤ 48 bytes |

`0x02` returns the state it actually applied rather than echoing the request, so
"enable a stream this build does not have" is a visible no-op rather than a silent lie.
`0x04` uses the `hwinfo` API (`CONFIG_HWINFO=y`), which on this SoC reads the FICR
`DEVICEID` words — a real per-chip identifier, not a build constant. `0x05` reports the
`VERSION` file's app version plus a `git describe` injected as a compile definition from
`CMakeLists.txt`, so a board can be traced back to a commit.

### battery and the status led

Battery voltage comes from the existing `vbatt` node via `VOLTAGE_DIVIDER_DT_SPEC_GET` and
`voltage_divider_scale_dt()`, which applies the declared 100k/200k ratio rather than a
hand-written factor of two. Percent is the same crude linear 3.2 V–4.2 V curve the
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
hundred and one.

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

The `uf2` variant is the right target: it sets `CONFIG_BUILD_OUTPUT_UF2=y`, and `west
flash` then copies `zephyr.uf2` to the bootloader's mass-storage drive. Enter the
bootloader by tapping the reset button twice quickly; a mass-storage device named
`FTHRSNSBOOT` appears on the host. The non-`uf2` `sense` variant flashes over SWD with pyocd or
J-Link instead, which this board exposes only as pads, so it is the harder path unless a
probe is already wired.

That variant also brings USB CDC console and shell along for free, via
`boards/common/usb/cdc_acm_serial.dtsi`. The host sees two ACM devices once the
application's own `cdc_acm_data` instance is added — one is the shell, the other is
binary sample data. Which `/dev/ttyACM*` is which follows enumeration order and should be
resolved by descriptor rather than assumed; opening the data one with a terminal is
harmless but useless.

`west build -t menuconfig` for the usual reasons.

## console shell

The shell runs on `board_cdc_acm_uart` — a USB CDC device, so `/dev/ttyACM0` with no
extra wiring:

```sh
pyserial-miniterm /dev/ttyACM0 115200      # ships in west's venv; Ctrl-] to quit
```

The baud rate is ignored — this is a USB CDC endpoint, not a real UART — but every tool
wants one, so 115200 is as good as any.

Beyond the built-ins, the application registers commands in the file that owns the data,
guarded by `#ifdef CONFIG_SHELL`, following both existing Zephyr areas. `CONFIG_I2C_SHELL`
earns its place here more than anywhere else in the repo:

```
i2c scan i2c@40003000                 # which parts this board revision actually has
i2c read_byte i2c@40003000 0x6a 0x0f  # WHO_AM_I: 0x69 = LSM6DS33, 0x6a = LSM6DS3TR-C
```

The device argument is the devicetree node's full name, not its label — `i2c0` is a label
and the shell will not accept it. Tab completion over the device list is the reliable way
to get it right. That second command is what answers the IMU question in [known
limitations](#known-limitations-and-open-questions). `CONFIG_SENSOR_SHELL` and
`CONFIG_ADC_SHELL` give a way to read every stream without a host program attached at all.

## tests

A `native_sim` ztest suite under `tests/`, built the way
`rpi_pico_zephyr_debug/tests/temp_convert/` is: the pure logic lives in translation units
free of Zephyr headers, and the *same* `.cpp` files are compiled into both the firmware and
the host test.

```sh
west build -b native_sim -p auto -d build_test tests/codec && ./build_test/zephyr/zephyr.exe
west twister -p native_sim -T tests
```

Two modules qualify, and only two — the rest of this firmware is hardware:

- `src/codec.cpp` — batch header pack/unpack and COBS encode/decode. The COBS tests should
  fuzz round trips and pin the 253/254/255-byte block-split boundaries explicitly; that is
  where every COBS implementation goes wrong, and the CircuitPython port's suite is the
  model.
- `src/battery_level.cpp` — divider millivolts to percent, and the hysteresis band
  function. The band function is pure and has a genuinely interesting property to test:
  that it moves in both directions and that no sequence of inputs can latch it.

## host side

Four programs in one nested pixi environment at `host/`, separate for the same reason
`microbit_v2_zephyr/host/` is: neither bleak nor rerun is a repo-wide dependency. They
come in a pair per transport plus a viewer, and share definitions through a plain import
rather than a second copy.

| Program | Transport | Does |
|---|---|---|
| `feather_protocol.py` | neither — no I/O | the wire format, the decoders, and the RPC client |
| `read_serial.py` | USB CDC, pyserial | measures the link |
| `read_ble.py` | BLE, bleak | measures the link |
| `feather_rerun.py` | either, by flag | plots what the other three decode |

`feather_protocol.py` is the **only** host-side definition of the wire format. Every
constant in it carries a `# Must match ../src/<file>.cpp: <SYMBOL>` comment naming the
firmware symbol it mirrors, as `microbit_v2_zephyr/host/ble_stream.py` does. It contains
no I/O: the two readers own their transports, and both hand bytes to the same decoder.

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
  `count - 1` is the number of intervals actually spanned.
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

### visualizing the streams

`feather_rerun.py` follows `microbit_v2_zephyr/host/ble_rerun.py`: `rr.init` then
`rr.spawn(memory_limit=...)` (not `rr.init(spawn=True)`, whose bool form cannot forward the
limit), an `rrb.Grid` of `rrb.TimeSeriesView`s built by a `build_blueprint`, static
`rr.SeriesLines` styling logged once, and `rr.set_time` + `rr.log(..., rr.Scalars(...))`
per sample. Unit conversion happens here, host-side, from the scale table the device
reported — there is no hard-coded conversion factor in the viewer.

Batched samples are back-dated using the batch's own `period_us`, so there is no
`--accel-batch-time` flag to choose and no nominal rate to assume.

## divergences from the circuitpython port

`~/code/remapy/adafruit_feather_sense/` runs on this same board. What changed, and why:

| | CircuitPython port | here | why |
|---|---|---|---|
| Framing | COBS on both transports | COBS on serial only | A GATT notification is already a datagram |
| Payload | one sensor sample per frame, `int32` pre-scaled | batches of raw `int16` | Requirement 3.2; and pre-scaling was pure cost on a 208 Hz path |
| Scales | a shared `SCALES` table in a file both sides import | reported over RPC | Requirement 5.3, and a range change stops needing a reflash |
| Timestamps | per-sample, host-derived rate | per-batch, plus `period_us` | Removes the viewer's spread-vs-arrival guess |
| Drop detection | inferred from timestamp spacing | explicit `seq` per stream | Separates device drops from link drops |
| Data channel | shared with the console | its own CDC ACM instance | The status-bar contamination bug cannot occur |
| Sampling | cooperative polling loop | FIFO watermark + preemptive threads | CircuitPython has no user-defined interrupt handlers; this is the port's single biggest unlock |
| Environmental | removed entirely (blocked ~152 ms/s) | kept, on the lowest-priority thread | Requirements 1.4–1.6 ask for them, and preemption contains what cooperative scheduling could not |
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

Most items here are claims this design has *not* verified, each with the check that would
settle it and the fallback if it goes the other way. The two that *were* settled — by the
test build described under [status](#status) — say so explicitly. Nothing else below should
be treated as known.

- **Which IMU is on this board is unknown.** Adafruit swapped the LSM6DS33 for the
  pin-compatible LSM6DS3TR-C in January 2024. `WHO_AM_I` (register `0x0f`) reads `0x69` for
  the DS33 and `0x6a` for the DS3TR-C — the latter is `LSM6DS3TR_C_ID` in
  `lsm6ds3tr-c_reg.h:178`. The CircuitPython port never determined it either; it probes at
  runtime and falls back. Zephyr's own board documentation
  (`boards/adafruit/feather_nrf52840/doc/index.rst`) lists the LSM6DS33, but that is the
  original part list, not evidence about any particular unit. **Check:**
  `i2c read_byte i2c@40003000 0x6a 0x0f` at the shell on first boot. **Fallback:** none needed —
  the out-of-tree driver accepts both ids, since the two parts share the register map this
  driver uses. But the number should be recorded here once it is known, so this paragraph
  can be deleted.

- **The NeoPixel's pin is assumed, not verified.** Nothing in Zephyr's board files declares
  it: the overlay above uses `P0.16` because that is what the `worldsemi,ws2812-gpio`
  binding's own example uses on an nRF board and what the Feather nRF52840 pinout reports.
  **Check:** the Adafruit pinout page, or drive the pin and watch. **Fallback:** correcting
  one number in `app.overlay`. If the pixel stays dark with no error, this is the first
  thing to doubt — a `ws2812-gpio` write to a wrong-but-valid pin fails silently.

- **Whether the IMU's INT1 is routed to a GPIO at all is unverified.** The CircuitPython
  port leaves this explicitly open, and the Zephyr board files say nothing because they do
  not describe the IMU at all. **Check:** the Adafruit schematic, and a scope or a
  `gpio get` on the candidate pin. **Fallback:** drain the FIFO on a `k_timer` instead of on
  a watermark IRQ. This matters less than it sounds: the sample *spacing* still comes from
  the chip's own clock either way, so `period_us` and the simultaneity argument both hold;
  what is lost is wake latency and a little CPU. Requirement 2 says "wherever possible", and
  if it is not possible this is why.

- **Whether the LIS3MDL's DRDY and the APDS9960's INT are routed is likewise unverified.**
  **Fallbacks:** both are automatic and were confirmed by the test build — with no
  `irq-gpios` and no `int-gpios` in the overlay, Kconfig resolved to
  `CONFIG_LIS3MDL_TRIGGER_NONE=y` and `CONFIG_APDS9960_FETCH_MODE_POLL=y` on its own.
  Neither fallback needs a code change, only an absent overlay property.

- **The SHT30 read cost is unmeasured.** Zephyr's `sht3xd` driver issues a blocking
  single-shot conversion. The CircuitPython port measured its environmental reads at
  ~152 ms per second of blocking and deleted them over it — though most of that was the
  BMP280, at ~45 ms per forced-mode conversion, and that chip is not used here. The SHT30
  alone may be far cheaper. It is quoted as an upper bound, not a prediction. Preemptive scheduling means a
  blocked env thread cannot delay the IMU, which is a real structural improvement, but it
  does not make the read cheaper and it does not prove the read is cheap. **Check:** time
  `sensor_sample_fetch` on the shell before trusting the 1 Hz budget. **Fallback:** the
  SHT3x also has a periodic mode; if the single-shot cost is bad, that is where to go.

- **The overlay builds; the IMU node in it does not exist yet.** Everything above was
  compiled and linked for `adafruit_feather_nrf52840/nrf52840/sense/uf2` **except** the
  `imu` node: the `scratchpad,lsm6ds3trc` compatible is created by the out-of-tree driver
  this document proposes and by nothing else, so it was omitted from the test build. The
  three in-tree sensor nodes, the TWIM change, the 400 kHz clock, the NeoPixel node and the
  second CDC ACM instance all resolved, and `CONFIG_I2C_NRFX_TWIM=y` confirms the DMA
  peripheral is the one actually bound.

- **`ws2812_gpio.c` does not compile in Zephyr 4.4.99 as this board configures it.** With
  `CONFIG_CLOCK_CONTROL_NRF` unset — which is the default here, since the board pulls in
  the newer split HFCLK/LFCLK drivers — the driver's `#else` branch at
  `drivers/led_strip/ws2812_gpio.c:139` calls `nrf_clock_control_release(drv_data->...)`
  against a `drv_data` that no longer exists in that function. **Fix:**
  `CONFIG_CLOCK_CONTROL_NRF=y`, which was verified to build. **If that ever stops being
  acceptable:** the same NeoPixel can be driven by `worldsemi,ws2812-spi` off `spi1`, at
  the cost of a byte of buffer per bit and one of the board's SPI pins, or the branch can
  be fixed upstream — it is a two-word patch.

- **No throughput number in this document was measured on this firmware.** The BLE and USB
  figures quoted are the CircuitPython port's, on the same board and the same host, and
  they are there as a *floor* and a calibration, not as a prediction. In particular, this
  design's expected BLE win comes from a 247-byte MTU and the 2M PHY, and neither has been
  demonstrated here.

- **Pressure and the microphone are deliberately out of scope.** The BMP280 is reachable —
  Zephyr's `bosch,bme280` driver accepts its chip id `0x58` at
  `drivers/sensor/bosch/bme280/bme280.c:358` — and the nRF52840's PDM peripheral has a
  driver and EasyDMA. Neither is in the requirements. If pressure is added later: put the
  BMP280 in normal mode so a read is a register fetch rather than a forced conversion,
  lower the pressure oversampling from its default, and derive altitude on the host rather
  than paying a second conversion for it.

- **The `dfg` integration named as future work needs no firmware change.** `dfg` has no
  concept of a free-running source node — a node with zero inputs is a validation error,
  and graph inputs are the only sources. A host reader is therefore an *application driving
  the graph*, calling `graph.inject(...)` then `run_until_idle()`, not a node inside it.
  The only thing this design owes that future is what it already provides: an integer
  sample timestamp per sample, which `dfg`'s nanosecond `Message.timestamp` wants
  multiplied by 1e6.
