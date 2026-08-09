"""Edge transports: the mechanism an edge uses to move messages.

In-memory buffers first. Shared-memory channels and sockets are a registration
away, and none of it changes the node API -- the transport is a property of an
edge, which is why it lives here and not in :mod:`dfg.node`.

Two details are load-bearing:

**The deque is created without ``maxlen``.** ``maxlen`` silently drops the oldest
element, which makes the ``error`` policy impossible to implement and hides drops
from the counter that is supposed to report them. Capacity is enforced by hand.

**``block`` is not an overflow policy.** It is the obvious fourth one and it
deadlocks a single-threaded scheduler instantly: the producer waits for space that
only the consumer can free, and the consumer only runs when the producer returns.
It becomes meaningful the first time a scheduler is concurrent, and not before.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from typing import Any, Protocol, runtime_checkable

from dfg.blueprint import EdgeTransport, Overflow
from dfg.errors import EdgeOverflowError, UnknownTransportError
from dfg.message import Envelope, Message


@dataclass(frozen=True, slots=True)
class EdgeConfig:
    """What a transport needs to know about the edge it serves.

    Attributes:
        key: A human-readable identifier, used in errors and control-plane stats.
        capacity: ``None`` for unbounded.
        on_overflow: What to do when a bounded queue is full.
    """

    key: str
    capacity: int | None = None
    on_overflow: Overflow = Overflow.ERROR


@runtime_checkable
class Transport(Protocol):
    """Moves messages along one edge.

    An input port has exactly one writer, so an input port's queue *is* its edge's
    transport. Graph inputs get a synthetic edge for the same reason: an injected
    message then travels the identical code path as a produced one, which is much
    of why a replay is trustworthy.
    """

    config: EdgeConfig

    def put(self, envelope: Envelope) -> None:
        """Enqueue, applying the overflow policy if the queue is full."""
        ...

    def get(self) -> Envelope:
        """Dequeue the oldest envelope."""
        ...

    def peek(self, index: int = 0) -> Envelope:
        """Return an envelope without consuming it. ``0`` is the oldest."""
        ...

    def clear(self) -> int:
        """Discard everything pending and return how many were discarded."""
        ...

    def __len__(self) -> int:
        """How many envelopes are pending."""
        ...

    @property
    def dropped(self) -> int:
        """How many envelopes an overflow policy has discarded."""
        ...


class InMemoryTransport:
    """An unbounded-by-default in-process queue.

    Unbounded is the default because a run under the single-threaded scheduler
    proceeds to quiescence, where a bound only converts a working graph into a
    failing one.
    """

    __slots__ = ("_queue", "_dropped", "config")

    def __init__(self, config: EdgeConfig) -> None:
        self.config = config
        # No maxlen: see the module docstring.
        self._queue: deque[Envelope] = deque()
        self._dropped = 0

    def put(self, envelope: Envelope) -> None:
        """Enqueue ``envelope``.

        Raises:
            EdgeOverflowError: If the edge is full and its policy is ``error``.
        """
        capacity = self.config.capacity
        if capacity is not None and len(self._queue) >= capacity:
            # Value patterns, so a config carrying the bare string still matches.
            match self.config.on_overflow:
                case Overflow.ERROR:
                    raise EdgeOverflowError(
                        f"edge {self.config.key!r} is full at capacity {capacity}; "
                        f"raise the capacity, or choose drop_oldest/drop_newest"
                    )
                case Overflow.DROP_OLDEST:
                    self._queue.popleft()
                    self._dropped += 1
                case Overflow.DROP_NEWEST:
                    self._dropped += 1
                    return
                case other:  # pragma: no cover - validation rejects these first
                    raise EdgeOverflowError(
                        f"edge {self.config.key!r} has unknown overflow policy "
                        f"{other!r}"
                    )
        self._queue.append(envelope)

    def get(self) -> Envelope:
        """Dequeue the oldest envelope.

        Raises:
            IndexError: If the queue is empty.
        """
        return self._queue.popleft()

    def peek(self, index: int = 0) -> Envelope:
        """Return a pending envelope without consuming it.

        Raises:
            IndexError: If ``index`` is out of range.
        """
        return self._queue[index]

    def clear(self) -> int:
        """Discard everything pending, returning the count. Not counted as drops."""
        count = len(self._queue)
        self._queue.clear()
        return count

    def __len__(self) -> int:
        return len(self._queue)

    @property
    def dropped(self) -> int:
        return self._dropped

    def __repr__(self) -> str:
        return (
            f"InMemoryTransport({self.config.key!r}, pending={len(self._queue)}, "
            f"dropped={self._dropped})"
        )


class PortQueueView:
    """A messages-only view of one input port, for readiness rules.

    Rules reason about messages; envelopes carry the control plane's enqueue clock
    and must never reach a node, which is what keeps sample time and latency from
    being conflated. This view is the seam: it unwraps on the way out and reports
    each dequeue to the control plane's callback.
    """

    __slots__ = ("_transport", "_on_dequeue")

    def __init__(
        self,
        transport: Transport,
        on_dequeue: Callable[[Transport, Envelope], None] | None = None,
    ) -> None:
        self._transport = transport
        self._on_dequeue = on_dequeue

    def __len__(self) -> int:
        return len(self._transport)

    def peek(self, index: int = 0) -> Message[Any]:
        """The pending message at ``index`` without consuming it."""
        return self._transport.peek(index).message

    def take(self, count: int) -> tuple[Message[Any], ...]:
        """Consume up to ``count`` messages, oldest first."""
        taken: list[Message[Any]] = []
        for _ in range(count):
            if len(self._transport) == 0:
                break
            envelope = self._transport.get()
            if self._on_dequeue is not None:
                self._on_dequeue(self._transport, envelope)
            taken.append(envelope.message)
        return tuple(taken)

    def __repr__(self) -> str:
        return f"PortQueueView({self._transport.config.key!r}, {len(self)} pending)"


type TransportFactory = Callable[[EdgeConfig], Transport]

# ``.value``, so the registry is keyed by plain strings: a member would compare and
# hash the same, but ``transport_names()`` and the unknown-transport error would
# report it by its enum repr rather than by the name an author writes.
MEMORY = EdgeTransport.MEMORY.value

_TRANSPORTS: dict[str, TransportFactory] = {MEMORY: InMemoryTransport}


def register_transport(name: str, factory: TransportFactory) -> None:
    """Register a transport factory under ``name``.

    Raises:
        ValueError: If the name is already registered.
    """
    if name in _TRANSPORTS:
        raise ValueError(f"transport {name!r} is already registered")
    _TRANSPORTS[name] = factory


def transport_names() -> tuple[str, ...]:
    """Registered transport names, sorted."""
    return tuple(sorted(_TRANSPORTS))


def is_registered_transport(name: str) -> bool:
    """Whether ``name`` names a registered transport."""
    return name in _TRANSPORTS


def make_transport(name: str, config: EdgeConfig) -> Transport:
    """Build the transport for one edge.

    Raises:
        UnknownTransportError: If ``name`` is not registered. This is checked at
            instantiation rather than at validation, because resolving it needs
            this registry and the blueprint layer does not import the runtime.
    """
    factory = _TRANSPORTS.get(name)
    if factory is None:
        raise UnknownTransportError(
            f"edge {config.key!r} uses transport {name!r}, which is not "
            f"registered; registered transports are {list(transport_names())}"
        )
    return factory(config)


def registered_transports() -> Mapping[str, TransportFactory]:
    """The transport registry, read-only-ish. For introspection and tests."""
    return dict(_TRANSPORTS)
