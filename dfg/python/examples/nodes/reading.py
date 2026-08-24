"""Reading nodes, with a small frozen dataclass of primitive fields as the payload.

Standard library only.

Payloads are whatever a node wants -- the framework never inspects one -- and the
simplest useful payload is a small frozen dataclass of primitive fields: a float, a
bool, a str, and an enum. It is cheap to construct, immutable, and comparable, which is
what lets a replay test assert equality on a whole output stream.

The nodes here do one obvious thing each to one field: scale the number, threshold it
into the flag, classify it into the level, and fold an external tag into the label.

Contrast with :mod:`examples.nodes.arrow`, where many such rows arrive at once in
columns, and :mod:`examples.nodes.signal`, whose payload is a multi-channel vector.
Same node contract, same scheduler.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, replace
from typing import Annotated, NamedTuple

from dfg.message import Message
from dfg.node import Emit, In, Node
from dfg.ports import Port
from dfg.registry import Registry


class Level(enum.Enum):
    """A coarse classification of a reading's magnitude.

    An enum, not a string, so a misclassification is a wrong *member* rather than a
    typo. It lives only inside a payload, never in a blueprint, so it never has to
    survive JSON -- see :mod:`dfg.serialize`.
    """

    LOW = enum.auto()
    MEDIUM = enum.auto()
    HIGH = enum.auto()


@dataclass(frozen=True, slots=True)
class Reading:
    """One reading: a measured value, a flag, a label, and a coarse level."""

    value: float
    active: bool
    label: str
    level: Level


class Scale(Node):
    """Applies an affine transform to ``value`` and leaves the other fields alone."""

    gain: float = 1.0
    offset: float = 0.0

    class Out(NamedTuple):
        scaled: Annotated[Emit[Reading], Port("Reading")]

    def run(self, *, raw: Annotated[In[Reading], Port("Reading")] = ()) -> Out:
        gain, offset = self.gain, self.offset
        out: list[Message[Reading]] = []
        for message in raw:
            reading = message.payload
            out.append(
                message.with_payload(
                    replace(reading, value=reading.value * gain + offset)
                )
            )
        return self.Out(scaled=tuple(out))


class Threshold(Node):
    """Sets ``active`` from whether ``value`` reaches ``threshold``.

    ``threshold`` is the natural thing for a parent graph to supply through a
    ``$param`` reference, which is how the same subgraph is reused at another cutoff.
    """

    threshold: float = 0.5

    class Out(NamedTuple):
        flagged: Annotated[Emit[Reading], Port("Reading")]

    def run(self, *, reading: Annotated[In[Reading], Port("Reading")] = ()) -> Out:
        threshold = self.threshold
        out: list[Message[Reading]] = []
        for message in reading:
            value = message.payload
            out.append(
                message.with_payload(replace(value, active=value.value >= threshold))
            )
        return self.Out(flagged=tuple(out))


class Grade(Node):
    """Classifies ``value`` into a :class:`Level`, keeping ``active`` from ``flagged``.

    Two input ports with ``all`` readiness, so it fires once per matched pair. Both
    ports are fed from the same stream, so they stay in step without the scheduler
    knowing anything about time.
    """

    low: float = 1.0
    high: float = 2.0

    class Out(NamedTuple):
        graded: Annotated[Emit[Reading], Port("Reading")]

    def run(
        self,
        *,
        reading: Annotated[In[Reading], Port("Reading")] = (),
        flagged: Annotated[In[Reading], Port("Reading")] = (),
    ) -> Out:
        low, high = self.low, self.high
        out: list[Message[Reading]] = []
        for reading_message, flagged_message in zip(reading, flagged):
            value = reading_message.payload
            if value.value >= high:
                level = Level.HIGH
            elif value.value >= low:
                level = Level.MEDIUM
            else:
                level = Level.LOW
            out.append(
                reading_message.with_payload(
                    replace(value, active=flagged_message.payload.active, level=level)
                )
            )
        return self.Out(graded=tuple(out))


class Relabel(Node):
    """Rewrites ``label`` to fold an external tag together with the graded level.

    Two input ports, one of them a separate graph input, so this is where a second
    stream joins the pipeline -- the ``all``-readiness pairing that ``core.Merge``
    exists to avoid needing for producers that share a port.
    """

    class Out(NamedTuple):
        labeled: Annotated[Emit[Reading], Port("Reading")]

    def run(
        self,
        *,
        reading: Annotated[In[Reading], Port("Reading")] = (),
        tag: In[str] = (),
    ) -> Out:
        out: list[Message[Reading]] = []
        for reading_message, tag_message in zip(reading, tag):
            value = reading_message.payload
            out.append(
                reading_message.with_payload(
                    replace(value, label=f"{tag_message.payload}:{value.level.name}")
                )
            )
        return self.Out(labeled=tuple(out))


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(Scale)
    registry.register(Threshold)
    registry.register(Grade)
    registry.register(Relabel)
    return registry
