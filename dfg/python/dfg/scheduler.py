"""The single-threaded run loop.

One step is: apply any queued parameter change, compute the ready set, pick one
ready node, consume its inputs, run it, publish what it produced. The scheduler
owns none of that state -- :class:`dfg.graph.Graph` builds the queues and provides
the publish step, so this module does not import the graph and the loop stays
readable on one screen.

The ready set is computed over the canonical topological order, so it is a
deterministic sequence before the ordering even sees it; the ordering then picks
from it, breaking ties by qualified node ID. See :mod:`dfg.ordering`.
"""

from __future__ import annotations

import logging
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from typing import Any

from dfg.control import ControlPlane
from dfg.errors import LifecycleError, NodeRunError
from dfg.flatten import FlatGraph, FlatNode
from dfg.message import Message
from dfg.node import Node, normalize_outputs
from dfg.ordering import Ordering
from dfg.readiness import PortQueue

logger = logging.getLogger("dfg")

type PortViews = Mapping[str, Mapping[str, PortQueue]]
"""Qualified node ID to port name to that port's pending-message view."""

type Publish = Callable[[str, Mapping[str, tuple[Message[Any], ...]]], None]
"""Called with a qualified node ID and what its ``run`` produced."""

type PublishError = Callable[[str, Message[Any]], None]
"""Called with a qualified node ID and an :class:`ErrorEvent` message."""


@dataclass(frozen=True, slots=True)
class ErrorEvent:
    """What the ``route`` error policy publishes.

    Attributes:
        qid: The qualified ID of the node whose ``run`` raised.
        exception_type: The exception's class name.
        message: ``str(exception)``.
    """

    qid: str
    exception_type: str
    message: str


class Scheduler:
    """Fires ready nodes one at a time, deterministically."""

    def __init__(
        self,
        *,
        flat: FlatGraph,
        nodes: Mapping[str, Node],
        views: PortViews,
        ordering: Ordering,
        control: ControlPlane,
        publish: Publish,
        publish_error: PublishError,
        order: tuple[str, ...],
    ) -> None:
        self._flat = flat
        self._nodes = nodes
        self._views = views
        self._ordering = ordering
        self._control = control
        self._publish = publish
        self._publish_error = publish_error
        self._order = order

    def ready(self) -> list[str]:
        """Qualified IDs that may fire now, in canonical topological order.

        Asking every node before picking one is why a readiness rule must not
        consume anything in ``is_ready``: most of the nodes asked will not run.
        """
        return [
            qid
            for qid in self._order
            if self._flat.nodes[qid].readiness.is_ready(self._views[qid])
        ]

    def step(self) -> bool:
        """Fire at most one node.

        Returns:
            ``True`` if a node fired, ``False`` if nothing was ready (or the graph
            is paused), which is what quiescence looks like.

        Raises:
            NodeRunError: If a node's ``run`` raised under the ``stop`` policy.
            NodeContractError: If a node returned something the output contract
                does not allow. Not subject to the error policy -- that is a bug in
                the node, not a bad message.
        """
        # Between firings, never during one.
        self._control._apply_pending_params()
        if self._control.paused:
            return False
        ready = self.ready()
        if not ready:
            return False

        qid = self._ordering.pick(ready)
        flat_node = self._flat.nodes[qid]
        inputs = flat_node.readiness.take(self._views[qid])
        try:
            produced = self._nodes[qid].run(inputs)
        except Exception as exc:
            self._handle_error(qid, flat_node, exc)
            return True
        outputs = normalize_outputs(produced, flat_node.outputs, where=qid)
        self._control._record_fired(qid)
        self._publish(qid, outputs)
        return True

    def run_until_idle(self, *, max_steps: int | None = None) -> int:
        """Fire ready nodes until none is ready.

        Args:
            max_steps: A guard against a runaway graph. ``None`` for no guard.

        Returns:
            How many nodes fired.

        Raises:
            LifecycleError: If ``max_steps`` was reached with work still ready.
        """
        steps = 0
        while self.step():
            steps += 1
            if max_steps is not None and steps >= max_steps:
                if self.ready() and not self._control.paused:
                    raise LifecycleError(
                        f"still {len(self.ready())} node(s) ready after "
                        f"max_steps={max_steps}; the graph may not reach quiescence"
                    )
                break
        return steps

    def _handle_error(self, qid: str, flat_node: FlatNode, exc: Exception) -> None:
        """Dispatch a failed ``run`` on the node's blueprint error policy.

        The consumed inputs are already gone in every case: they were taken before
        ``run`` was called, so a ``drop`` really does drop this message rather than
        retrying it forever.
        """
        self._control._record_error(qid)
        match flat_node.on_error:
            case "drop":
                logger.exception(
                    "node %s dropped a message after %s: %s",
                    qid,
                    type(exc).__name__,
                    exc,
                    exc_info=exc,
                )
            case "route":
                logger.warning(
                    "node %s routed an error: %s: %s", qid, type(exc).__name__, exc
                )
                event = ErrorEvent(qid, type(exc).__name__, str(exc))
                self._publish_error(qid, Message(event, self._control.now()))
            case _:
                # "stop" is the default because a silent continue hides bugs in
                # exactly the case where nobody is watching -- an offline batch run
                # over a large recording.
                raise NodeRunError(
                    f"node {qid!r} ({flat_node.type}) raised "
                    f"{type(exc).__name__}: {exc}"
                ) from exc
