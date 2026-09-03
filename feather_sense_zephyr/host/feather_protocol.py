"""The Feather Sense wire format: decoders, the scale table, and the RPC client.

This is the **only** host-side definition of the wire format. Every constant
below names the firmware symbol it mirrors, so a change on one side has an
obvious place to land on the other. The two readers and the rerun viewer import
this module; they do not restate any of it.

There is no I/O here. ``read_serial.py`` and ``read_ble.py`` own their
transports and hand bytes to the decoders below, which is what lets the same
decoder run against a live link, a BLE notification, and a test fixture.

The format, in one place::

    batch = [ t_ms:u32 ][ seq:u16 ][ period_us:u16 ][ stream_id:u8 ][ count:u8 ]
            [ sample x count ]

Little-endian throughout. ``t_ms`` is the device uptime at the **first** sample
in the batch; ``period_us`` is the spacing within it, from the chip's own output
data rate, so batched samples are back-dated from the device's clock rather than
from a nominal rate the host guessed. ``seq`` counts batches per stream and
wraps at 16 bits: a gap in ``seq`` is a device-side drop, and a gap in ``t_ms``
without one is a link-side drop.

**Nothing here knows what a sample means in SI units.** The device reports that
over RPC, per field, as ``value_in_nano_SI = raw * num / den`` (see
:class:`ScaleTable`). That is the cost of letting the IMU stream carry the
sensor's own int16 registers untouched: a capture cannot be decoded without the
scales, so both readers fetch them at connect and any recorder must store them
alongside the samples. The benefit is that changing the accelerometer's
full-scale range is a fact the host learns at runtime rather than a constant it
must be reflashed to agree with.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator, NamedTuple

# --- streams -----------------------------------------------------------------
# Must match ../src/codec.hpp: StreamId
STREAM_IMU = 1
STREAM_MAGN = 2
STREAM_ENV = 3
STREAM_BATTERY = 4
STREAM_BUTTON = 5

STREAM_NAMES = {
    STREAM_IMU: "imu",
    STREAM_MAGN: "magn",
    STREAM_ENV: "env",
    STREAM_BATTERY: "battery",
    STREAM_BUTTON: "button",
}

# Field names per stream, in wire order. The scale table reports one entry per
# field in this same order, so the two zip together.
# Must match ../src/rpc.cpp: kImuScales, kMagnScales, kEnvScales, kBatteryScales,
# kButtonScales
STREAM_FIELDS = {
    STREAM_IMU: ("gx", "gy", "gz", "ax", "ay", "az"),
    STREAM_MAGN: ("x", "y", "z"),
    STREAM_ENV: ("temperature", "humidity", "light"),
    STREAM_BATTERY: ("millivolts", "percent", "flags"),
    STREAM_BUTTON: ("code", "pressed", "pad"),
}

# struct formats for one sample body, per stream.
# Must match ../src/codec.hpp: kImuSampleBytes and the rest, and the Sample
# structs in ../src/imu.cpp, magn.cpp, env.cpp, battery.cpp and buttons.cpp.
STREAM_SAMPLE_FORMAT = {
    STREAM_IMU: "<6h",
    STREAM_MAGN: "<3h",
    STREAM_ENV: "<hHH",
    STREAM_BATTERY: "<HBB",
    STREAM_BUTTON: "<HBB",
}

STREAM_SAMPLE_BYTES = {
    stream: struct.calcsize(fmt) for stream, fmt in STREAM_SAMPLE_FORMAT.items()
}

# Must match ../src/codec.hpp: kBatteryFlagUsb
BATTERY_FLAG_USB = 0x01

# --- batch header ------------------------------------------------------------
# Must match ../src/codec.hpp: kBatchHeaderBytes and pack_batch_header()
_HEADER_FORMAT = "<IHHBB"
HEADER_BYTES = struct.calcsize(_HEADER_FORMAT)
assert HEADER_BYTES == 10


class BatchHeader(NamedTuple):
    t_ms: int
    seq: int
    period_us: int
    stream_id: int
    count: int


@dataclass(frozen=True)
class Batch:
    """A decoded batch: its header, plus one tuple of raw values per sample."""

    header: BatchHeader
    samples: list[tuple[int, ...]]

    @property
    def stream_id(self) -> int:
        return self.header.stream_id

    @property
    def name(self) -> str:
        return STREAM_NAMES.get(self.header.stream_id, f"stream{self.header.stream_id}")

    def timestamps_ms(self) -> list[float]:
        """One device timestamp per sample, back-dated from the batch header.

        ``t_ms`` is the first sample's instant and ``period_us`` the spacing, so
        this needs no nominal rate and no arrival time. The CircuitPython port's
        viewer had an ``--accel-batch-time={spread,arrival}`` flag precisely
        because it had to guess this; sending ``period_us`` removes the guess.
        """
        step_ms = self.header.period_us / 1000.0
        return [self.header.t_ms + i * step_ms for i in range(len(self.samples))]


def parse_batch(payload: bytes) -> Batch:
    """Parse one batch. Raises ValueError on anything structurally wrong."""
    if len(payload) < HEADER_BYTES:
        raise ValueError(f"batch of {len(payload)} bytes is shorter than a header")

    header = BatchHeader(*struct.unpack_from(_HEADER_FORMAT, payload, 0))

    fmt = STREAM_SAMPLE_FORMAT.get(header.stream_id)
    if fmt is None:
        raise ValueError(f"unknown stream id {header.stream_id}")

    size = STREAM_SAMPLE_BYTES[header.stream_id]
    body = payload[HEADER_BYTES:]
    if len(body) != header.count * size:
        raise ValueError(
            f"{STREAM_NAMES[header.stream_id]} batch declares {header.count} samples "
            f"({header.count * size} bytes) but carries {len(body)}"
        )

    samples = [struct.unpack_from(fmt, body, i * size) for i in range(header.count)]

    return Batch(header=header, samples=samples)


# --- COBS --------------------------------------------------------------------
# Must match ../src/codec.cpp: cobs_encode() and cobs_decode()
_COBS_BLOCK = 0xFE  # 254: the longest run of non-zero bytes one code byte spans


def cobs_encode(data: bytes) -> bytes:
    """Encode so the result contains no 0x00, leaving 0x00 free as a delimiter."""
    out = bytearray()
    for chunk in data.split(b"\x00"):
        # A run of 254 needs splitting across code bytes: 0xFF means "254
        # non-zero bytes, and no zero followed". That distinction is the whole
        # of the block boundary, and it is what the firmware's tests/codec pins.
        while len(chunk) >= _COBS_BLOCK:
            out.append(0xFF)
            out += chunk[:_COBS_BLOCK]
            chunk = chunk[_COBS_BLOCK:]
        out.append(len(chunk) + 1)
        out += chunk
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """Inverse of :func:`cobs_encode`. Raises ValueError on a malformed frame."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        if code == 0 or i + code > n:
            raise ValueError("malformed COBS frame")
        i += 1
        out += data[i : i + code - 1]
        i += code - 1
        if code < 0xFF and i < n:
            out.append(0)
    return bytes(out)


# --- serial framing ----------------------------------------------------------
# On the serial link both sample batches and RPC frames share one pipe, so a
# frame carries one leading byte saying which it is. BLE needs no equivalent:
# there the characteristic identifies the channel and a notification is already
# a delimited datagram. The payload bytes are identical on both transports.
# Must match ../src/codec.hpp: Channel
CHANNEL_SAMPLES = 1
CHANNEL_RPC = 2


def frame(channel: int, payload: bytes) -> bytes:
    """Wrap a payload for the serial link: cobs([channel] + payload) + 0x00."""
    return cobs_encode(bytes([channel]) + payload) + b"\x00"


class FrameDecoder:
    """Accumulates serial bytes and yields ``(channel, payload)`` pairs.

    Malformed frames are counted in :attr:`errors` rather than raised, so a
    glitchy stream resynchronises at the next delimiter. That counter should
    read 0: this is a dedicated CDC ACM endpoint that carries nothing but these
    frames, so anything else is contamination of the data channel rather than
    noise to tune out.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self.errors = 0

    def feed(self, chunk: bytes) -> Iterator[tuple[int, bytes]]:
        self._buf += chunk
        while True:
            idx = self._buf.find(0)
            if idx < 0:
                break
            raw = bytes(self._buf[:idx])
            del self._buf[: idx + 1]
            if not raw:
                continue  # a leading or doubled delimiter
            try:
                decoded = cobs_decode(raw)
            except ValueError:
                self.errors += 1
                continue
            if not decoded:
                self.errors += 1
                continue
            yield decoded[0], decoded[1:]


# --- rpc ---------------------------------------------------------------------
# Must match ../src/codec.hpp: Opcode
OP_GET_BATTERY = 0x01
OP_SET_STREAM = 0x02
OP_GET_SCALE = 0x03
OP_GET_SERIAL = 0x04
OP_GET_BUILD_ID = 0x05

# Must match ../src/codec.hpp: Unit
UNIT_NAMES = {
    1: "m/s^2",
    2: "rad/s",
    3: "T",
    4: "degC",
    5: "%RH",
    6: "lx",
    7: "V",
    8: "%",
    9: "",
}

# Must match ../src/codec.hpp: kScaleFieldBytes
_SCALE_FIELD_FORMAT = "<Bii"
SCALE_FIELD_BYTES = struct.calcsize(_SCALE_FIELD_FORMAT)
assert SCALE_FIELD_BYTES == 9


class Scale(NamedTuple):
    """value_in_SI = raw * num / den / 1e9."""

    unit: int
    num: int
    den: int

    @property
    def unit_name(self) -> str:
        return UNIT_NAMES.get(self.unit, f"unit{self.unit}")

    def to_si(self, raw: int) -> float:
        return raw * self.num / self.den / 1e9


@dataclass
class ScaleTable:
    """Per-stream scales, one entry per sample field in wire order."""

    by_stream: dict[int, list[Scale]] = field(default_factory=dict)

    def decode(self, batch: Batch) -> list[dict[str, float]]:
        """Convert a batch's raw samples to SI, one dict per sample."""
        scales = self.by_stream.get(batch.stream_id)
        names = STREAM_FIELDS[batch.stream_id]
        if scales is None:
            raise ValueError(
                f"no scales for {batch.name}; fetch them with get_scale first"
            )
        return [
            {name: scale.to_si(raw) for name, scale, raw in zip(names, scales, sample)}
            for sample in batch.samples
        ]

    def describe(self) -> str:
        lines = []
        for stream_id in sorted(self.by_stream):
            names = STREAM_FIELDS[stream_id]
            for name, scale in zip(names, self.by_stream[stream_id]):
                unit = scale.unit_name or "(raw)"
                lines.append(
                    f"  {STREAM_NAMES[stream_id]:<8} {name:<12} "
                    f"{scale.num}/{scale.den} nano-{unit}"
                )
        return "\n".join(lines)


def build_request(seq: int, opcode: int, args: bytes = b"") -> bytes:
    """[ seq:u8 ][ opcode:u8 ][ args ]. Must match ../src/rpc.cpp: handle()"""
    return bytes([seq & 0xFF, opcode]) + args


class RpcResponse(NamedTuple):
    seq: int
    opcode: int
    status: int
    payload: bytes

    @property
    def ok(self) -> bool:
        return self.status == 0


def parse_response(data: bytes) -> RpcResponse:
    """[ seq:u8 ][ opcode:u8 ][ status:i8 ][ payload ]."""
    if len(data) < 3:
        raise ValueError(f"rpc response of {len(data)} bytes is shorter than a header")
    seq, opcode, status = struct.unpack_from("<BBb", data, 0)
    return RpcResponse(seq=seq, opcode=opcode, status=status, payload=data[3:])


def parse_scale_payload(payload: bytes) -> tuple[int, list[Scale]]:
    """Parse a `get scale` reply: [ stream_id:u8 ][ n:u8 ][ n x scale ]."""
    if len(payload) < 2:
        raise ValueError("scale payload is too short")
    stream_id, count = payload[0], payload[1]
    expected = 2 + count * SCALE_FIELD_BYTES
    if len(payload) < expected:
        raise ValueError(
            f"scale payload declares {count} fields ({expected} bytes) "
            f"but carries {len(payload)}"
        )
    scales = [
        Scale(
            *struct.unpack_from(_SCALE_FIELD_FORMAT, payload, 2 + i * SCALE_FIELD_BYTES)
        )
        for i in range(count)
    ]
    return stream_id, scales


def parse_battery_payload(payload: bytes) -> tuple[int, int, int]:
    """Parse a `get battery` reply: [ mv:u16 ][ percent:u8 ][ flags:u8 ]."""
    if len(payload) < 4:
        raise ValueError("battery payload is too short")
    return struct.unpack_from("<HBB", payload, 0)


# --- BLE ---------------------------------------------------------------------
# One custom 128-bit vendor primary service. Four notify characteristics, one
# per rate class, so a host can subscribe to only what it needs, plus the RPC
# pair. Must match ../src/ble.cpp: FEATHER_UUID
def _uuid(n: int) -> str:
    return f"f5e5{n:04x}-4a75-4b21-9d3e-6b1c2a7e0000"


BLE_DEVICE_NAME = "feather-sense"  # Must match ../prj.conf: CONFIG_BT_DEVICE_NAME
UUID_SERVICE = _uuid(0)
UUID_IMU = _uuid(1)
UUID_MAGN = _uuid(2)
UUID_ENV = _uuid(3)
UUID_EVENTS = _uuid(4)
UUID_RPC_REQUEST = _uuid(5)
UUID_RPC_RESPONSE = _uuid(6)

# Which characteristic carries which stream. Battery and button share the events
# one: both are rare and neither needs its own subscription.
BLE_STREAM_CHARACTERISTICS = {
    UUID_IMU: (STREAM_IMU,),
    UUID_MAGN: (STREAM_MAGN,),
    UUID_ENV: (STREAM_ENV,),
    UUID_EVENTS: (STREAM_BATTERY, STREAM_BUTTON),
}


# --- rate statistics ---------------------------------------------------------
class StreamStats:
    """Per-stream rate accounting for one reporting window.

    The reporting rule is inherited whole from the CircuitPython port, because
    getting it wrong there over-reported by ~10 % and every number that port
    published was wrong in the flattering direction:

    - ``dev`` -- ``(count - 1) * 1000 / (last_ts - first_ts)``, from *device*
      timestamps. This is the number to quote. It needs no clock synchronisation
      between board and host, and ``count - 1`` is the number of intervals
      actually spanned rather than the number of samples.
    - ``host`` -- ``count / measured elapsed``. It only tells you the link kept
      up. Divided by *measured* elapsed, never by the nominal window: a window
      gated on ``>= 1.0 s`` is always overshot, and a true 91 Hz stream then
      reports "100/s".
    - ``gap max`` -- the largest device-timestamp interval seen. A rate on
      target with an outsized gap means the stream stalled and caught up in a
      burst, which neither average shows.
    - ``seq gaps`` -- what separates a device-side drop from a link-side one.
      On the CircuitPython port this could only be inferred from the shape of
      the timestamp spacing.

    Every one of those is also accumulated across windows, under a ``total_``
    name, and :meth:`reset` leaves those alone. ``seq wraps`` exists only in the
    run totals: at ~20.8 IMU batches/s the 16-bit counter takes about 52 minutes
    to roll over, so it is an event no reporting window is long enough to
    contain and no run before this one was long enough to reach.
    """

    def __init__(self, name: str) -> None:
        self.name = name
        self.samples = 0
        self.batches = 0
        self.first_ts: float | None = None
        self.last_ts: float | None = None
        self.max_gap_ms = 0.0
        self.seq_gaps = 0
        self._prev_seq: int | None = None
        self._prev_ts: float | None = None
        self._prev_batch_t_ms: int | None = None

        # Run totals, which `reset()` deliberately leaves alone. A soak needs
        # the whole run's figures and the windowed ones are gone by the time it
        # ends -- printing 5400 windowed lines and no total is the shape of
        # report that answers "was it fine just now" and never "was it fine".
        self.total_samples = 0
        self.total_batches = 0
        self.total_seq_gaps = 0
        self.total_max_gap_ms = 0.0
        self.seq_wraps = 0
        self.restarts = 0
        self.run_first_ts: float | None = None
        self.run_last_ts: float | None = None
        self._run_prev_ts: float | None = None

    def add(self, batch: Batch) -> None:
        # A device reboot restarts `seq` and `t_ms` together, and would read as
        # a 16-bit wrap plus an enormous gap -- which would make "seq wraps 1"
        # unfalsifiable, since the one thing a long run is trying to establish
        # would also be what a reboot printed. `t_ms` going backwards is what
        # separates them: a real wrap leaves the uptime clock running.
        restarted = (
            self._prev_batch_t_ms is not None
            and batch.header.t_ms < self._prev_batch_t_ms
        )
        if restarted:
            self.restarts += 1

        if self._prev_seq is not None and not restarted:
            expected = (self._prev_seq + 1) & 0xFFFF
            if batch.header.seq != expected:
                gap = (batch.header.seq - expected) & 0xFFFF
                self.seq_gaps += gap
                self.total_seq_gaps += gap
            # A wrap is the one case where `seq` legitimately goes backwards.
            # Counting it lets a long run *report* that 16 bits rolled over,
            # rather than resting on the masked arithmetic above being right
            # about an event nothing has ever seen happen.
            if batch.header.seq < self._prev_seq:
                self.seq_wraps += 1
        self._prev_seq = batch.header.seq
        self._prev_batch_t_ms = batch.header.t_ms

        for ts in batch.timestamps_ms():
            if self.first_ts is None:
                self.first_ts = ts
            if self.run_first_ts is None:
                self.run_first_ts = ts
            if self._prev_ts is not None:
                self.max_gap_ms = max(self.max_gap_ms, ts - self._prev_ts)
            # Tracked from its own predecessor because `reset()` clears
            # `_prev_ts`: the windowed figure cannot see an interval that
            # straddles a window boundary, and a stall is not less real for
            # having started just before the second ticked over.
            if self._run_prev_ts is not None:
                self.total_max_gap_ms = max(
                    self.total_max_gap_ms, ts - self._run_prev_ts
                )
            self._prev_ts = ts
            self._run_prev_ts = ts
            self.last_ts = ts
            self.run_last_ts = ts
            self.samples += 1
            self.total_samples += 1

        self.batches += 1
        self.total_batches += 1

    @property
    def device_rate(self) -> float:
        """Samples per second from device timestamps. 0.0 until two arrive."""
        if self.first_ts is None or self.last_ts is None or self.samples < 2:
            return 0.0
        span_ms = self.last_ts - self.first_ts
        if span_ms <= 0:
            return 0.0
        return (self.samples - 1) * 1000.0 / span_ms

    def host_rate(self, elapsed_s: float) -> float:
        return self.samples / elapsed_s if elapsed_s > 0 else 0.0

    def total_host_rate(self, elapsed_s: float) -> float:
        """Arrival rate over the whole run, against *measured* run elapsed."""
        return self.total_samples / elapsed_s if elapsed_s > 0 else 0.0

    def reset(self) -> None:
        """Start a new reporting window.

        Keeps the sequence continuity and every ``total_`` field: a window is a
        reporting boundary, not a run boundary.
        """
        self.samples = 0
        self.batches = 0
        self.first_ts = None
        self.last_ts = None
        self.max_gap_ms = 0.0
        self.seq_gaps = 0
        self._prev_ts = None

    def line(self, elapsed_s: float) -> str:
        return (
            f"  {self.name:<8} dev {self.device_rate:7.2f}/s  "
            f"host {self.host_rate(elapsed_s):7.2f}/s  "
            f"batches {self.batches:5d}  "
            f"gap max {self.max_gap_ms:7.1f} ms  "
            f"seq gaps {self.seq_gaps}"
        )

    @property
    def total_device_rate(self) -> float:
        """Device-timestamp rate over the whole run. 0.0 until two arrive.

        The same ``(count - 1) / span`` rule as :attr:`device_rate`, over the
        run's first and last timestamps instead of the window's. Note that
        ``t_ms`` wraps at 32 bits (49.7 days); a run long enough to see that
        would report a negative span here and is not what this is for.

        A device restart mid-run spans the discontinuity and makes this figure
        meaningless, which is why :attr:`restarts` is printed beside it rather
        than quietly absorbed.
        """
        if self.run_first_ts is None or self.run_last_ts is None:
            return 0.0
        if self.total_samples < 2:
            return 0.0
        span_ms = self.run_last_ts - self.run_first_ts
        if span_ms <= 0:
            return 0.0
        return (self.total_samples - 1) * 1000.0 / span_ms

    def total_line(self, elapsed_s: float) -> str:
        return (
            f"  {self.name:<8} dev {self.total_device_rate:7.2f}/s  "
            f"host {self.total_host_rate(elapsed_s):7.2f}/s  "
            f"samples {self.total_samples:9d}  "
            f"batches {self.total_batches:8d}  "
            f"gap max {self.total_max_gap_ms:7.1f} ms  "
            f"seq gaps {self.total_seq_gaps}  "
            f"seq wraps {self.seq_wraps}  "
            f"restarts {self.restarts}"
        )
