"""Signal nodes, with a small frozen dataclass as a multi-channel payload.

Standard library only.

A :class:`Sample` is a generic three-channel vector -- nothing about it is tied to any
particular sensor. It exists so the numpy and pyarrow examples have a small, immutable,
comparable payload to accumulate: :mod:`examples.nodes.arrow` batches many samples into
columns, and ``video_pipeline`` runs a stream of samples through :class:`Integrate` to
drive an overlay.

Contrast with :mod:`examples.nodes.reading`, whose payload is a single scalar reading.
Same node contract, same scheduler.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Annotated, NamedTuple

from dfg.message import Message
from dfg.node import Emit, In, Node
from dfg.ports import Port
from dfg.registry import Registry


@dataclass(frozen=True, slots=True)
class Sample:
    """One sample of a generic three-channel signal."""

    x: float
    y: float
    z: float


class Integrate(Node):
    """Accumulates the ``x`` channel into a running scalar, one output per input.

    A tiny bit of state -- the running sum -- so the scalar grows over the stream,
    which is what makes a downstream overlay visibly move. ``rate_hz`` sets the step so
    the integral is in the signal's own units rather than per-sample.
    """

    rate_hz: float = 200.0

    class Out(NamedTuple):
        track: Annotated[Emit[float], Port("track")]

    def setup(self) -> None:
        self._acc = 0.0

    def run(self, *, sample: Annotated[In[Sample], Port("Sample")] = ()) -> Out:
        dt = 1.0 / self.rate_hz
        out: list[Message[float]] = []
        for message in sample:
            self._acc += message.payload.x * dt
            out.append(message.with_payload(self._acc))
        return self.Out(track=tuple(out))


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(Integrate)
    return registry
