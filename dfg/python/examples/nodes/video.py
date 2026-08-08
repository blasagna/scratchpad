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

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, ClassVar

import numpy as np

from dfg.message import Message
from dfg.node import Inputs, Node, Outputs
from dfg.ports import PortSpec
from dfg.registry import Registry

FRAME_RGB = "frame_rgb"
FRAME_GRAY = "frame_gray"


@dataclass(frozen=True, slots=True)
class FrameStats:
    """Luma summary of one frame."""

    mean: float
    peak: int
    bright_pixels: int


class ToGray(Node):
    """Converts RGB to luma with the Rec. 601 weights."""

    INPUTS = (PortSpec("in", type_tag=FRAME_RGB),)
    OUTPUTS = (PortSpec("out", type_tag=FRAME_GRAY),)

    WEIGHTS: ClassVar[tuple[float, float, float]] = (0.299, 0.587, 0.114)

    def run(self, inputs: Inputs) -> Outputs:
        weights = np.array(self.WEIGHTS, dtype=np.float32)
        out: list[Message[np.ndarray]] = []
        for message in inputs.get("in", ()):
            frame = np.asarray(message.payload)
            luma = frame.astype(np.float32) @ weights
            out.append(message.with_payload(np.round(luma).astype(np.uint8)))
        return {"out": out}


class OverlayBox(Node):
    """Draws a marker whose position comes from a pose.

    Takes ``(frame, pose)`` pairs -- the tuple :class:`examples.nodes.core.Resample`
    produces -- and tints a fixed-size box whose vertical position tracks the pose's
    roll. Only the box's pixels change, so a test can check exactly that.
    """

    INPUTS = (PortSpec("in", type_tag="frame_and_pose"),)
    OUTPUTS = (PortSpec("out", type_tag=FRAME_RGB),)
    PARAMS: ClassVar[Mapping[str, Any]] = {
        "size": 6,
        "colour": (0, 255, 0),
        "gain": 40.0,
    }

    def run(self, inputs: Inputs) -> Outputs:
        size = self.params["size"]
        colour = np.array(self.params["colour"], dtype=np.uint8)
        gain = self.params["gain"]
        out: list[Message[np.ndarray]] = []
        for message in inputs.get("in", ()):
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
        return {"out": out}


class FrameStatsNode(Node):
    """Summarizes a grey frame into a small dataclass.

    A frame in, a handful of floats out -- so a recording of this output is small
    enough to keep, which is the usual reason to compute it inside the graph rather
    than after it.
    """

    INPUTS = (PortSpec("in", type_tag=FRAME_GRAY),)
    OUTPUTS = (PortSpec("out", type_tag="FrameStats"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"bright_threshold": 128}

    def run(self, inputs: Inputs) -> Outputs:
        threshold = self.params["bright_threshold"]
        out: list[Message[FrameStats]] = []
        for message in inputs.get("in", ()):
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
        return {"out": out}


class Downscale(Node):
    """Halves a frame's resolution by averaging non-overlapping blocks.

    Only exact factors are supported: cropping or padding to make an inexact factor
    work is a decision a caller should make on purpose, not one a node should make
    quietly.
    """

    INPUTS = (PortSpec("in", type_tag=FRAME_RGB),)
    OUTPUTS = (PortSpec("out", type_tag=FRAME_RGB),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"factor": 2}

    def run(self, inputs: Inputs) -> Outputs:
        factor = self.params["factor"]
        out: list[Message[np.ndarray]] = []
        for message in inputs.get("in", ()):
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
        return {"out": out}


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register("video.to_gray", ToGray)
    registry.register("video.overlay_box", OverlayBox)
    registry.register("video.frame_stats", FrameStatsNode)
    registry.register("video.downscale", Downscale)
    return registry
