"""IMU nodes, with small frozen dataclasses as payloads. Standard library only.

Payloads are whatever a node wants -- the framework never inspects one -- and a
dataclass is the right shape for a single sample from a sensor: a handful of named
floats, cheap to construct, immutable, and comparable, which is what lets a replay
test assert equality on a whole output stream.

Contrast with :mod:`examples.nodes.arrow`, where the same data arrives 64 rows at a
time in columns. Same node contract, same scheduler.
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass, replace
from typing import Annotated, Any, NamedTuple

from dfg.message import Message
from dfg.node import Emit, In, Node
from dfg.ports import Port
from dfg.registry import Registry

GRAVITY_M_S2 = 9.80665


@dataclass(frozen=True, slots=True)
class ImuSample:
    """One sample from a 6-axis IMU: acceleration in m/s^2, rate in rad/s."""

    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float


@dataclass(frozen=True, slots=True)
class Orientation:
    """A roll/pitch estimate in radians, plus the accelerometer magnitude used."""

    roll: float
    pitch: float
    accel_magnitude: float


@dataclass(frozen=True, slots=True)
class ImuStats:
    """Summary of a run of samples."""

    count: int
    mean_accel_magnitude: float
    max_gyro_magnitude: float


class Calibrate(Node):
    """Subtracts a per-axis bias and applies a scale factor.

    ``rate_hz`` is carried but unused by the arithmetic: it is here because it is
    the natural thing for a parent graph to supply through a ``$param`` reference,
    and a subgraph whose parameters are decorative is not worth having.
    """

    # Sequence rather than tuple: JSON has one sequence type, so a bias written
    # as a tuple comes back from a serialized blueprint as a list.
    accel_bias: Sequence[float] = (0.0, 0.0, 0.0)
    gyro_bias: Sequence[float] = (0.0, 0.0, 0.0)
    scale: float = 1.0
    rate_hz: float = 200.0

    class Out(NamedTuple):
        corrected: Annotated[Emit[ImuSample], Port("ImuSample")]

    def run(self, *, raw: Annotated[In[ImuSample], Port("ImuSample")] = ()) -> Out:
        abx, aby, abz = self.accel_bias
        gbx, gby, gbz = self.gyro_bias
        scale = self.scale
        out: list[Message[ImuSample]] = []
        for message in raw:
            sample = message.payload
            out.append(
                message.with_payload(
                    ImuSample(
                        ax=(sample.ax - abx) * scale,
                        ay=(sample.ay - aby) * scale,
                        az=(sample.az - abz) * scale,
                        gx=sample.gx - gbx,
                        gy=sample.gy - gby,
                        gz=sample.gz - gbz,
                    )
                )
            )
        return self.Out(corrected=tuple(out))


class Predict(Node):
    """Integrates gyro rates into a dead-reckoned roll/pitch state.

    The "predict" half of a complementary filter: fast, drifts.
    """

    rate_hz: float = 200.0

    class Out(NamedTuple):
        state: Annotated[Emit[Orientation], Port("Orientation")]

    def setup(self) -> None:
        self._roll = 0.0
        self._pitch = 0.0

    def run(self, *, imu: Annotated[In[ImuSample], Port("ImuSample")] = ()) -> Out:
        dt = 1.0 / self.rate_hz
        out: list[Message[Orientation]] = []
        for message in imu:
            sample = message.payload
            self._roll += sample.gx * dt
            self._pitch += sample.gy * dt
            magnitude = math.sqrt(sample.ax**2 + sample.ay**2 + sample.az**2)
            out.append(
                message.with_payload(Orientation(self._roll, self._pitch, magnitude))
            )
        return self.Out(state=tuple(out))


class Update(Node):
    """Corrects a predicted state towards the accelerometer's gravity direction.

    Two input ports with ``all`` readiness, so it fires once per matched pair. Both
    ports are fed from the same 200 Hz stream, so they stay in step without the
    scheduler knowing anything about time.
    """

    alpha: float = 0.02

    class Out(NamedTuple):
        fused: Annotated[Emit[Orientation], Port("Orientation")]

    def run(
        self,
        *,
        imu: Annotated[In[ImuSample], Port("ImuSample")] = (),
        state: Annotated[In[Orientation], Port("Orientation")] = (),
    ) -> Out:
        alpha = self.alpha
        pairs = zip(imu, state)
        out: list[Message[Orientation]] = []
        for imu_message, state_message in pairs:
            sample = imu_message.payload
            predicted = state_message.payload
            measured_roll = math.atan2(sample.ay, sample.az)
            measured_pitch = math.atan2(
                -sample.ax, math.sqrt(sample.ay**2 + sample.az**2)
            )
            fused = Orientation(
                roll=(1 - alpha) * predicted.roll + alpha * measured_roll,
                pitch=(1 - alpha) * predicted.pitch + alpha * measured_pitch,
                accel_magnitude=predicted.accel_magnitude,
            )
            out.append(state_message.with_payload(fused))
        return self.Out(fused=tuple(out))


class GravitySplit(Node):
    """Splits acceleration into a gravity estimate and the rest.

    Two output ports, which is the other half of why ``run`` returns a mapping.
    """

    alpha: float = 0.1

    class Out(NamedTuple):
        gravity: Annotated[Emit[ImuSample], Port("ImuSample")]
        linear: Annotated[Emit[ImuSample], Port("ImuSample")]

    def setup(self) -> None:
        self._gravity = (0.0, 0.0, GRAVITY_M_S2)

    def run(self, *, imu: Annotated[In[ImuSample], Port("ImuSample")] = ()) -> Out:
        alpha = self.alpha
        gravity_out: list[Message[ImuSample]] = []
        linear_out: list[Message[ImuSample]] = []
        for message in imu:
            sample = message.payload
            self._gravity = tuple(
                (1 - alpha) * held + alpha * measured
                for held, measured in zip(
                    self._gravity, (sample.ax, sample.ay, sample.az)
                )
            )
            gx, gy, gz = self._gravity
            gravity_out.append(
                message.with_payload(replace(sample, ax=gx, ay=gy, az=gz))
            )
            linear_out.append(
                message.with_payload(
                    replace(
                        sample, ax=sample.ax - gx, ay=sample.ay - gy, az=sample.az - gz
                    )
                )
            )
        return self.Out(gravity=tuple(gravity_out), linear=tuple(linear_out))


class Stats(Node):
    """Summarizes a whole window of samples into one message.

    Paired with :class:`examples.nodes.core.Window` this is a many-in-one-out node,
    which is what the ``count`` readiness rule and the windowing node exist for.
    """

    class Out(NamedTuple):
        stats: Annotated[Emit[ImuStats], Port("ImuStats")]

    def run(
        self,
        *,
        window: Annotated[In[tuple[Message[ImuSample], ...]], Port("window")] = (),
    ) -> Out:
        out: list[Message[ImuStats]] = []
        for message in window:
            samples = [inner.payload for inner in message.payload]
            magnitudes = [math.sqrt(s.ax**2 + s.ay**2 + s.az**2) for s in samples]
            gyros = [math.sqrt(s.gx**2 + s.gy**2 + s.gz**2) for s in samples]
            out.append(
                message.with_payload(
                    ImuStats(
                        count=len(samples),
                        mean_accel_magnitude=sum(magnitudes) / len(magnitudes),
                        max_gyro_magnitude=max(gyros),
                    )
                )
            )
        return self.Out(stats=tuple(out))


class Overlay(Node):
    """Combines a frame with a pose into a text description.

    Stands in for drawing on a frame, so the IMU example needs no numpy. The real
    thing is :func:`examples.nodes.video.OverlayBox`.
    """

    class Out(NamedTuple):
        composited: Emit[str]

    def run(
        self,
        *,
        frame: In[Any] = (),
        pose: Annotated[In[Orientation], Port("Orientation")] = (),
    ) -> Out:
        pairs = zip(frame, pose)
        return self.Out(
            composited=tuple(
                frame_message.with_payload(
                    f"{frame_message.payload} roll={pose_message.payload.roll:+.4f} "
                    f"pitch={pose_message.payload.pitch:+.4f}"
                )
                for frame_message, pose_message in pairs
            )
        )


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(Calibrate)
    registry.register(Predict)
    registry.register(Update)
    registry.register(GravitySplit)
    registry.register(Stats)
    registry.register(Overlay)
    return registry
