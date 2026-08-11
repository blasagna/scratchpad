"""Messages and time.

A message is a payload plus a timestamp, and the timestamp is the *sample* time --
when the data was captured. The framework never interprets it: it is set by
whoever injects the data and propagated by nodes as a matter of convention.

**A message carries no wall-clock field on purpose.** The latency the control
plane reports is wall-clock time spent in the graph, and reading the sample
timestamp for it reports garbage the moment a recording is replayed faster than
real time. So the transport wraps a message in an :class:`Envelope` stamped from
the control plane's own clock, and because an envelope never reaches a node, a
node author cannot conflate the two even by accident.

Timestamps are integer nanoseconds rather than float seconds or ``datetime``:
exact, comparable, hashable, cheap to write into a recording, and they let a
replay test assert equality instead of approximate equality.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

NANOS_PER_SECOND: int = 1_000_000_000

type Timestamp = int
"""Sample time in integer nanoseconds. The epoch is the injector's choice."""


@dataclass(frozen=True, slots=True)
class Message[T]:
    """A payload and the sample time it was captured at.

    Attributes:
        payload: Any Python object. The framework never inspects it.
        timestamp: Sample time in nanoseconds, in the injector's epoch.
    """

    payload: T
    timestamp: Timestamp

    def with_payload[U](self, payload: U) -> Message[U]:
        """Return a new message carrying ``payload`` at this message's sample time.

        This is the propagation convention in one call, so a node that transforms
        a payload does not have to remember to pass the timestamp along.
        """
        return Message(payload, self.timestamp)


@dataclass(frozen=True, slots=True)
class Envelope:
    """A message in transit, plus when it was enqueued.

    Attributes:
        message: The message an author's node will see.
        enqueued_ns: The control plane's *own* clock at enqueue -- never the
            sample time. Edge latency is ``clock() - enqueued_ns`` at dequeue.
    """

    message: Message[Any]
    enqueued_ns: int


def ts_from_seconds(seconds: float) -> Timestamp:
    """Convert a float number of seconds to integer nanoseconds."""
    return round(seconds * NANOS_PER_SECOND)


def ts_to_seconds(timestamp: Timestamp) -> float:
    """Convert integer nanoseconds to a float number of seconds."""
    return timestamp / NANOS_PER_SECOND


def ts_from_sample_index(index: int, rate_hz: float) -> Timestamp:
    """Return the sample time of sample ``index`` in a stream at ``rate_hz``.

    Rounded to the nearest nanosecond, which keeps a synthesized 200 Hz stream's
    timestamps exact (5 ms is a whole number of nanoseconds) and any other rate
    reproducible.

    Raises:
        ValueError: If ``rate_hz`` is not positive.
    """
    if rate_hz <= 0.0:
        raise ValueError(f"rate_hz must be positive, got {rate_hz!r}")
    return round(index * NANOS_PER_SECOND / rate_hz)
