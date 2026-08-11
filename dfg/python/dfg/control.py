"""The control plane: configuration, flow control, and measurement.

Data and control are separate planes. Data is the flow of inputs and processed
outputs; control is everything here -- parameter changes, starting and stopping,
queue depth per edge, latency, resetting pending data, and flow control.

**Latency is wall-clock time spent in the graph, measured against this plane's own
clock. It never reads a message's timestamp.** A message's timestamp is the sample
time, and conflating the two is the obvious shortcut that reports garbage the moment
a recording is replayed faster than real time: the sample times are then hours apart
while the actual processing took milliseconds. The clock is injectable so a test can
assert exact tick counts.

Parameter changes are applied **between** ``run`` invocations, never during one.
Mutating a node's parameters underneath a running ``run`` is the natural-looking
implementation and it makes every node author responsible for their own locking.
"""

from __future__ import annotations

import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any

from dfg.errors import ImmutableParamError, ParamError
from dfg.message import Envelope
from dfg.node import Node
from dfg.transport import Transport

type Clock = Callable[[], int]
"""Returns a monotonically non-decreasing integer nanosecond count."""

default_clock: Clock = time.perf_counter_ns


@dataclass(slots=True)
class EdgeStats:
    """Counters and latency for one edge.

    Attributes:
        depth: How many messages are pending right now.
        enqueued: Total ever put on this edge.
        dequeued: Total ever taken off it.
        dropped: Total discarded by the overflow policy.
        latency_ns_last: Wall-clock nanoseconds the last dequeued message spent
            waiting on this edge. Not derived from any sample timestamp.
        latency_ns_max: The largest such wait seen.
        latency_ns_total: The sum, so a mean is ``total / dequeued``.
    """

    depth: int = 0
    enqueued: int = 0
    dequeued: int = 0
    dropped: int = 0
    latency_ns_last: int = 0
    latency_ns_max: int = 0
    latency_ns_total: int = 0

    @property
    def latency_ns_mean(self) -> float:
        """Mean wait in nanoseconds, or ``0.0`` before anything was dequeued."""
        return self.latency_ns_total / self.dequeued if self.dequeued else 0.0


@dataclass(slots=True)
class NodeStats:
    """Counters and lifecycle state for one node.

    Attributes:
        fired: How many times ``run`` returned normally.
        errors: How many times ``run`` raised, whatever the policy did next.
        setup_done: Whether ``setup`` completed without raising.
        teardown_done: Whether ``teardown`` has been called.
    """

    fired: int = 0
    errors: int = 0
    setup_done: bool = False
    teardown_done: bool = False


@dataclass(slots=True)
class _PendingChange:
    """A queued parameter change, waiting for the gap between two firings."""

    qid: str
    changes: dict[str, Any] = field(default_factory=dict)


class ControlPlane:
    """Inspect and steer a running graph.

    Obtained from :attr:`dfg.graph.Graph.control`; not built directly.
    """

    def __init__(
        self,
        *,
        nodes: Mapping[str, Node],
        transports: Mapping[str, Transport],
        clock: Clock = default_clock,
    ) -> None:
        self._nodes = nodes
        self._transports = transports
        self._clock = clock
        self._edge_stats = {key: EdgeStats() for key in transports}
        self._node_stats = {qid: NodeStats() for qid in nodes}
        self._pending: list[_PendingChange] = []
        self._paused = False

    # --- Measurement ---------------------------------------------------------

    @property
    def clock(self) -> Clock:
        """The control plane's own clock. The only clock latency is measured with."""
        return self._clock

    def now(self) -> int:
        """Read the clock."""
        return self._clock()

    def edge_stats(self) -> Mapping[str, EdgeStats]:
        """Per-edge counters, keyed by edge key. Depth is refreshed on the way out."""
        for key, transport in self._transports.items():
            stats = self._edge_stats[key]
            stats.depth = len(transport)
            stats.dropped = transport.dropped
        return MappingProxyType(self._edge_stats)

    def node_stats(self) -> Mapping[str, NodeStats]:
        """Per-node counters, keyed by qualified node ID."""
        return MappingProxyType(self._node_stats)

    def queue_depth(self, edge_key: str) -> int:
        """How many messages are pending on one edge.

        Note that a subgraph boundary has no queue of its own: flattening turns a
        parent edge into one queue per inner target, so depth is per flattened edge.

        Raises:
            KeyError: If ``edge_key`` is not an edge of this graph.
        """
        return len(self._transports[edge_key])

    def total_pending(self) -> int:
        """Messages pending across every edge. Zero means the graph is quiescent."""
        return sum(len(transport) for transport in self._transports.values())

    def edge_keys(self) -> tuple[str, ...]:
        """Every edge key, sorted."""
        return tuple(sorted(self._transports))

    # --- Flow control --------------------------------------------------------

    @property
    def paused(self) -> bool:
        """Whether the scheduler is currently refusing to fire anything."""
        return self._paused

    def pause(self) -> None:
        """Stop firing nodes. Injection still works; messages just queue up."""
        self._paused = True

    def resume(self) -> None:
        """Resume firing nodes."""
        self._paused = False

    def reset_pending(self) -> int:
        """Discard every queued message, returning how many were discarded.

        Not counted as drops: a drop is something a policy did to a graph that was
        trying to work, and this is the operator saying to start over.
        """
        discarded = 0
        for key, transport in self._transports.items():
            count = transport.clear()
            discarded += count
            self._edge_stats[key].depth = 0
        return discarded

    # --- Configuration -------------------------------------------------------

    def set_params(self, qid: str, changes: Mapping[str, Any]) -> None:
        """Queue a parameter change, applied before the next firing.

        Raises:
            KeyError: If ``qid`` is not a node of this graph.
            ParamError: If a name is not a declared parameter of that node.
            ImmutableParamError: If a name is not in the node's ``MUTABLE_PARAMS``.
                Checked here, immediately, so the caller learns at the call rather
                than whenever the scheduler next runs.
        """
        node = self._nodes[qid]
        for name in changes:
            if name not in node.PARAMS:
                raise ParamError(
                    f"node {qid!r} ({type(node).__name__}) has no parameter "
                    f"{name!r}; declared parameters are {sorted(node.PARAMS)}"
                )
            if name not in node.MUTABLE_PARAMS:
                raise ImmutableParamError(
                    f"node {qid!r} parameter {name!r} is immutable; a node opts a "
                    f"parameter into live changes by listing it in MUTABLE_PARAMS"
                )
        self._pending.append(_PendingChange(qid, dict(changes)))

    def params(self, qid: str) -> Mapping[str, Any]:
        """A node's current parameters.

        Raises:
            KeyError: If ``qid`` is not a node of this graph.
        """
        return self._nodes[qid].params

    def has_pending_params(self) -> bool:
        """Whether any queued parameter change is waiting to be applied."""
        return bool(self._pending)

    # --- Called by the scheduler ---------------------------------------------

    def _apply_pending_params(self) -> None:
        """Apply queued changes. Called between firings, never during one."""
        if not self._pending:
            return
        pending, self._pending = self._pending, []
        for change in pending:
            self._nodes[change.qid]._apply_param_changes(change.changes)

    def _record_enqueue(self, transport: Transport) -> None:
        stats = self._edge_stats[transport.config.key]
        stats.enqueued += 1
        stats.depth = len(transport)

    def _record_dequeue(self, transport: Transport, envelope: Envelope) -> None:
        """Record a dequeue and the wall-clock time the message spent waiting."""
        stats = self._edge_stats[transport.config.key]
        stats.dequeued += 1
        stats.depth = len(transport)
        waited = self._clock() - envelope.enqueued_ns
        stats.latency_ns_last = waited
        stats.latency_ns_max = max(stats.latency_ns_max, waited)
        stats.latency_ns_total += waited

    def _record_fired(self, qid: str) -> None:
        self._node_stats[qid].fired += 1

    def _record_error(self, qid: str) -> None:
        self._node_stats[qid].errors += 1

    def _record_setup(self, qid: str) -> None:
        self._node_stats[qid].setup_done = True

    def _record_teardown(self, qid: str) -> None:
        self._node_stats[qid].teardown_done = True
