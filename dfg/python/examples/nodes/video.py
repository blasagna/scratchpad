"""Video nodes over numpy arrays.

A payload here is an ``H x W x 3`` ``uint8`` frame, or an ``H x W`` ``uint8`` frame
once converted to grey. Frames are the payload that makes the "any Python object"
rule worth having: nothing about them fits in a message header, and the framework
never looks inside one.

Video is also where the framework's refusal to align time shows up honestly. A 30 fps
frame stream and a 200 Hz pose stream do not line up, and no scheduler here tries to
make them: :class:`examples.nodes.core.Resample` is an ordinary node that holds the
newest pose, and :class:`OverlayBox` consumes the pair it produces.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from typing import Annotated, ClassVar, NamedTuple, Protocol

import numpy as np

from dfg.message import Message
from dfg.node import Emit, In, Node
from dfg.ports import Port
from dfg.registry import Registry

FRAME_RGB = "frame_rgb"
FRAME_GRAY = "frame_gray"


class Pose(Protocol):
    """What :class:`OverlayBox` needs of a pose: somewhere to put the box.

    A protocol rather than an import of :class:`examples.nodes.imu.Orientation`,
    because the drawing has no business knowing where the angle came from -- and
    because these examples are meant to stay independent of one another.
    """

    @property
    def roll(self) -> float: ...


@dataclass(frozen=True, slots=True)
class FrameStats:
    """Luma summary of one frame."""

    mean: float
    peak: int
    bright_pixels: int


class ToGray(Node):
    """Converts RGB to luma with the Rec. 601 weights."""

    # ClassVar, so it is a constant rather than a parameter: nobody reconfigures
    # the Rec. 601 weights per instance.
    WEIGHTS: ClassVar[tuple[float, float, float]] = (0.299, 0.587, 0.114)

    class Out(NamedTuple):
        output: Annotated[Emit[np.ndarray], Port(FRAME_GRAY)]

    def run(self, *, input: Annotated[In[np.ndarray], Port(FRAME_RGB)] = ()) -> Out:
        weights = np.array(self.WEIGHTS, dtype=np.float32)
        out: list[Message[np.ndarray]] = []
        for message in input:
            frame = np.asarray(message.payload)
            luma = frame.astype(np.float32) @ weights
            out.append(message.with_payload(np.round(luma).astype(np.uint8)))
        return self.Out(output=tuple(out))


class OverlayBox(Node):
    """Draws a marker whose position comes from a pose.

    Takes ``(frame, pose)`` pairs -- the tuple :class:`examples.nodes.core.Resample`
    produces -- and tints a fixed-size box whose vertical position tracks the pose's
    roll. Only the box's pixels change, so a test can check exactly that.
    """

    size: int = 6
    colour: Sequence[int] = (0, 255, 0)
    gain: float = 40.0

    class Out(NamedTuple):
        output: Annotated[Emit[np.ndarray], Port(FRAME_RGB)]

    def run(
        self,
        *,
        input: Annotated[In[tuple[np.ndarray, Pose]], Port("frame_and_pose")] = (),
    ) -> Out:
        size = self.size
        colour = np.array(self.colour, dtype=np.uint8)
        gain = self.gain
        out: list[Message[np.ndarray]] = []
        for message in input:
            frame, pose = message.payload
            frame = np.asarray(frame)
            height, width = frame.shape[0], frame.shape[1]
            # Roll maps to a row, clamped so the box always lands inside the frame.
            centre = int(height / 2 + pose.roll * gain)
            top = max(0, min(height - size, centre - size // 2))
            left = max(0, min(width - size, width // 2 - size // 2))
            marked = frame.copy()
            marked[top : top + size, left : left + size] = colour
            out.append(message.with_payload(marked))
        return self.Out(output=tuple(out))


class FrameStatsNode(Node):
    """Summarizes a grey frame into a small dataclass.

    A frame in, a handful of floats out -- so a recording of this output is small
    enough to keep, which is the usual reason to compute it inside the graph rather
    than after it.
    """

    bright_threshold: int = 128

    class Out(NamedTuple):
        output: Annotated[Emit[FrameStats], Port("FrameStats")]

    def run(self, *, input: Annotated[In[np.ndarray], Port(FRAME_GRAY)] = ()) -> Out:
        threshold = self.bright_threshold
        out: list[Message[FrameStats]] = []
        for message in input:
            frame = np.asarray(message.payload)
            out.append(
                message.with_payload(
                    FrameStats(
                        mean=round(float(frame.mean()), 4),
                        peak=int(frame.max()),
                        bright_pixels=int(np.count_nonzero(frame >= threshold)),
                    )
                )
            )
        return self.Out(output=tuple(out))


class Downscale(Node):
    """Halves a frame's resolution by averaging non-overlapping blocks.

    Only exact factors are supported: cropping or padding to make an inexact factor
    work is a decision a caller should make on purpose, not one a node should make
    quietly.
    """

    factor: int = 2

    class Out(NamedTuple):
        output: Annotated[Emit[np.ndarray], Port(FRAME_RGB)]

    def run(self, *, input: Annotated[In[np.ndarray], Port(FRAME_RGB)] = ()) -> Out:
        factor = self.factor
        out: list[Message[np.ndarray]] = []
        for message in input:
            frame = np.asarray(message.payload)
            height, width = frame.shape[0], frame.shape[1]
            if height % factor or width % factor:
                raise ValueError(
                    f"frame {height}x{width} is not divisible by factor {factor}; "
                    f"crop or pad before downscaling"
                )
            reshaped = frame.reshape(
                height // factor, factor, width // factor, factor, -1
            )
            averaged = reshaped.mean(axis=(1, 3))
            out.append(message.with_payload(np.round(averaged).astype(np.uint8)))
        return self.Out(output=tuple(out))


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(ToGray)
    registry.register(OverlayBox)
    registry.register(FrameStatsNode)
    registry.register(Downscale)
    return registry
