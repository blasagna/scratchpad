"""The runnable graph: instantiation, lifecycle, and the application's API.

The application drives a graph through this in-memory API: it injects data into
graph inputs and retrieves data from graph outputs, by polling or by callback.
Topics are the third way in, and they are a *tap*: subscribing observes an output
port for debugging, recording, and visualization while the messages still move over
each edge's own transport. A tap is something you can decline to install; a broker
in the hot path is not.

Instantiating validates, flattens, builds one node per leaf and one transport per
flattened edge, and computes the canonical order. It runs no ``setup``: that is
:meth:`Graph.start`, so a graph can be inspected before it acquires anything.
"""

from __future__ import annotations

import logging
from collections.abc import Callable, Iterator, Mapping, Sequence
from types import MappingProxyType, TracebackType
from typing import Any

from dfg.blueprint import GraphSpec
from dfg.control import Clock, ControlPlane, default_clock
from dfg.errors import LifecycleError, NodeSetupError
from dfg.flatten import Endpoint, FlatEdge, FlatGraph, flatten, topological_order
from dfg.message import Envelope, Message
from dfg.node import Node
from dfg.ordering import LevelOrdering, Ordering, TopologicalOrdering, make_ordering
from dfg.ports import ERROR_PORT, topic_of
from dfg.readiness import PortQueue
from dfg.registry import Registry
from dfg.scheduler import ErrorEvent, Scheduler
from dfg.transport import (
    MEMORY,
    EdgeConfig,
    PortQueueView,
    Transport,
    make_transport,
)
from dfg.validate import validate

logger = logging.getLogger("dfg")

type MessageCallback = Callable[[str, Message[Any]], None]
"""Called with the name subscribed to and one message."""


class Subscription:
    """A live tap on an output port. Cancel it when you are done.

    Because the tap is at the *port* and not at each edge, a subscriber sees each
    message exactly once however many nodes consume it -- fan-out from one output
    is still one topic.
    """

    __slots__ = ("name", "endpoint", "callback", "_graph", "_cancelled")

    def __init__(
        self,
        graph: Graph,
        name: str,
        endpoint: Endpoint,
        callback: MessageCallback,
    ) -> None:
        self.name = name
        self.endpoint = endpoint
        self.callback = callback
        self._graph = graph
        self._cancelled = False

    @property
    def cancelled(self) -> bool:
        """Whether this subscription has been cancelled."""
        return self._cancelled

    def cancel(self) -> None:
        """Stop receiving messages. Idempotent."""
        if not self._cancelled:
            self._graph._remove_tap(self)
            self._cancelled = True

    def __enter__(self) -> Subscription:
        return self

    def __exit__(self, *exc: object) -> None:
        self.cancel()

    def __repr__(self) -> str:
        return f"Subscription({self.name!r}, cancelled={self._cancelled})"


class Graph:
    """A blueprint instantiated into something that can process data."""

    def __init__(
        self,
        *,
        spec: GraphSpec,
        flat: FlatGraph,
        nodes: dict[str, Node],
        ordering: Ordering,
        clock: Clock,
    ) -> None:
        self._spec = spec
        self._flat = flat
        self._nodes = nodes
        self._ordering = ordering
        self._order = topological_order(flat)

        self._inbox: dict[Endpoint, Transport] = {}
        self._sinks: dict[str, Transport] = {}
        self._transports: dict[str, Transport] = {}
        self._build_transports()

        self._control = ControlPlane(
            nodes=nodes, transports=self._transports, clock=clock
        )
        self._views: dict[str, dict[str, PortQueue]] = {
            qid: {
                port.name: PortQueueView(
                    self._inbox[(qid, port.name)], self._control._record_dequeue
                )
                for port in node.inputs
            }
            for qid, node in flat.nodes.items()
        }
        edges_by_src: dict[Endpoint, list[FlatEdge]] = {}
        for edge in flat.edges:
            edges_by_src.setdefault(edge.src, []).append(edge)
        self._outgoing: dict[Endpoint, tuple[Transport, ...]] = {
            (qid, port.name): tuple(
                self._inbox[edge.dst]
                for edge in sorted(
                    edges_by_src.get((qid, port.name), ()), key=lambda e: e.dst
                )
            )
            for qid, node in flat.nodes.items()
            for port in node.outputs
        }
        self._output_sinks: dict[Endpoint, tuple[str, ...]] = {}
        for name, endpoint in flat.outputs.items():
            self._output_sinks.setdefault(endpoint, ())
            self._output_sinks[endpoint] += (name,)

        self._taps: dict[Endpoint, list[Subscription]] = {}
        self._output_callbacks: dict[str, list[MessageCallback]] = {}
        self._started = False
        self._stopped = False

        if isinstance(self._ordering, (TopologicalOrdering, LevelOrdering)):
            self._ordering.prepare(flat, order=self._order)
        else:
            self._ordering.prepare(flat)
        self._scheduler = Scheduler(
            flat=flat,
            nodes=nodes,
            views=self._views,
            ordering=self._ordering,
            control=self._control,
            publish=self._publish,
            publish_error=self._publish_error,
            order=self._order,
        )

    # --- Construction --------------------------------------------------------

    @classmethod
    def instantiate(
        cls,
        spec: GraphSpec,
        registry: Registry,
        *,
        ordering: Ordering | str | None = None,
        clock: Clock = default_clock,
    ) -> Graph:
        """Build a runnable graph from a blueprint.

        Args:
            spec: The blueprint.
            registry: Resolves each node type name to a factory.
            ordering: An :class:`~dfg.ordering.Ordering`, a registered ordering
                name, or ``None`` for ``topological``.
            clock: The control plane's clock, for latency. Injectable so a test can
                assert exact ticks. Never used for message timestamps.

        Returns:
            A graph on which nothing has been set up yet.

        Raises:
            ValidationError: If the blueprint is not valid. Every problem is
                attached, including unknown node types -- which is what a blueprint
                deserialized without a registry produces.
            UnknownTransportError: If an edge names an unregistered transport.
        """
        validate(spec, registry)
        flat = flatten(spec, registry)
        nodes = {
            qid: registry.create(node.type_name, node.params)
            for qid, node in flat.nodes.items()
        }
        if ordering is None:
            resolved = TopologicalOrdering()
        elif isinstance(ordering, str):
            resolved = make_ordering(ordering)
        else:
            resolved = ordering
        return cls(spec=spec, flat=flat, nodes=nodes, ordering=resolved, clock=clock)

    def _build_transports(self) -> None:
        """One transport per input port, plus one sink per root output.

        Every declared input port gets a queue, even one with no writer, so a
        readiness rule can always index every port by name. A graph input's targets
        get one too, which is what makes an injected message travel the same code
        path as a produced one.
        """
        writers: dict[Endpoint, str] = {}
        for edge in self._flat.edges:
            if edge.dst in writers:
                # Validation rejects fan-in, so reaching here means a bug in
                # flattening rather than a bad blueprint. Say so plainly.
                raise LifecycleError(
                    f"input port {edge.dst[0]}.{edge.dst[1]} has two writers "
                    f"({writers[edge.dst]} and {edge.src[0]}.{edge.src[1]}) after "
                    f"flattening; an input port takes one writer"
                )
            writers[edge.dst] = f"{edge.src[0]}.{edge.src[1]}"
            self._add_transport(
                edge.dst,
                EdgeConfig(edge.key, edge.capacity, edge.on_overflow),
                edge.transport,
            )

        injected: dict[Endpoint, str] = {}
        for name, endpoints in self._flat.inputs.items():
            for endpoint in endpoints:
                injected[endpoint] = name

        for qid, node in self._flat.nodes.items():
            for port in node.inputs:
                endpoint = (qid, port.name)
                if endpoint in self._inbox:
                    continue
                source = injected.get(endpoint)
                key = (
                    f"input:{source} -> {qid}.{port.name}"
                    if source
                    else f"unwired -> {qid}.{port.name}"
                )
                self._add_transport(endpoint, EdgeConfig(key), MEMORY)

        for name, endpoint in self._flat.outputs.items():
            key = f"{endpoint[0]}.{endpoint[1]} -> output:{name}"
            sink = make_transport(MEMORY, EdgeConfig(key))
            self._sinks[name] = sink
            self._transports[key] = sink

    def _add_transport(
        self, endpoint: Endpoint, config: EdgeConfig, transport_name: str
    ) -> None:
        transport = make_transport(transport_name, config)
        self._inbox[endpoint] = transport
        self._transports[config.key] = transport

    # --- Lifecycle -----------------------------------------------------------

    def start(self) -> None:
        """Call ``setup`` on every node, exactly once, in topological order.

        Raises:
            LifecycleError: If the graph has already been started.
            NodeSetupError: If a node's ``setup`` raised. The graph does not start.
                The raising node does **not** get a ``teardown``; every node already
                set up does, in reverse topological order.
        """
        if self._started:
            raise LifecycleError(
                "this graph has already been started; rebuild the blueprint and "
                "instantiate again rather than restarting"
            )
        self._started = True
        for qid in self._order:
            try:
                self._nodes[qid].setup()
            except Exception as exc:
                # The raiser gets no teardown: it never finished acquiring, so it
                # has no business being asked to release.
                # A failing teardown here must not mask the setup failure that is
                # the actual news.
                self._stop_without_masking()
                raise NodeSetupError(
                    f"node {qid!r} ({self._flat.nodes[qid].type_name}) raised "
                    f"{type(exc).__name__} during setup: {exc}"
                ) from exc
            self._control._record_setup(qid)

    def stop(self) -> None:
        """Call ``teardown`` on every node that was set up, in reverse order.

        Reverse topological order means a node tears down before anything it
        depends on. Safe to call more than once, and safe to call on a graph that
        never started.
        """
        if self._stopped:
            return
        self._stopped = True
        self._teardown_set_up_nodes()

    def _teardown_set_up_nodes(self) -> None:
        """Tear down, in reverse topological order, whatever finished ``setup``.

        A node that never ran ``setup`` never runs ``teardown``. Every node gets its
        chance even if one raises here, because ``teardown`` is the only place a node
        is promised, and one node's bad cleanup must not strand another's.
        """
        stats = self._control.node_stats()
        # Exception, not BaseException: the except clause below catches Exception, and
        # ExceptionGroup only carries that. A KeyboardInterrupt during teardown should
        # propagate rather than be collected.
        failures: list[Exception] = []
        for qid in reversed(self._order):
            if not stats[qid].setup_done or stats[qid].teardown_done:
                continue
            try:
                self._nodes[qid].teardown()
            except Exception as exc:  # noqa: BLE001 - every node still gets a turn
                logger.exception("node %s raised during teardown", qid, exc_info=exc)
                failures.append(exc)
            finally:
                self._control._record_teardown(qid)
        if failures:
            raise ExceptionGroup("teardown failed", failures)

    def __enter__(self) -> Graph:
        self.start()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        # teardown runs on a clean stop *and* on an error stop.
        self.stop()

    @property
    def started(self) -> bool:
        """Whether ``setup`` has been attempted."""
        return self._started

    @property
    def stopped(self) -> bool:
        """Whether ``teardown`` has run."""
        return self._stopped

    # --- Running -------------------------------------------------------------

    def step(self) -> bool:
        """Fire at most one node. ``False`` means nothing was ready.

        Raises:
            LifecycleError: If the graph has not been started, or has stopped.
            NodeRunError: If a node raised under the ``stop`` policy. ``teardown``
                still runs before this propagates.
        """
        self._require_running()
        return self._guard(self._scheduler.step)

    def run_until_idle(self, *, max_steps: int | None = None) -> int:
        """Fire ready nodes until none is ready. Returns how many fired.

        Raises:
            LifecycleError: If the graph is not running, or ``max_steps`` was hit
                with work still ready.
            NodeRunError: If a node raised under the ``stop`` policy.
        """
        self._require_running()
        return self._guard(lambda: self._scheduler.run_until_idle(max_steps=max_steps))

    def _guard[T](self, call: Callable[[], T]) -> T:
        """Run the scheduler, tearing the graph down if anything escapes."""
        try:
            return call()
        except BaseException:
            self._stop_without_masking()
            raise

    def _stop_without_masking(self) -> None:
        """Tear down while an exception is already propagating.

        A failing ``teardown`` must not replace the error that caused the stop: the
        node that actually broke is what the caller needs to see, and a cleanup
        failure discovered on the way out is secondary. It is still logged, and
        ``stop()`` called directly does raise.
        """
        try:
            self.stop()
        except Exception as exc:  # noqa: BLE001 - the original error wins
            logger.exception("teardown failed while stopping on an error", exc_info=exc)

    def _require_running(self) -> None:
        if not self._started:
            raise LifecycleError("call start() (or use the graph as a context manager)")
        if self._stopped:
            raise LifecycleError("this graph has stopped; instantiate a new one")

    def ready(self) -> list[str]:
        """Qualified IDs of the nodes that may fire now, in canonical order."""
        return self._scheduler.ready()

    # --- Data in and out -----------------------------------------------------

    def inject(self, input_name: str, message: Message[Any]) -> None:
        """Put a message on a graph input.

        Graph inputs are the only sources, so this is the only way data enters --
        which is most of what makes a recorded input sequence fully determine a run.

        Raises:
            KeyError: If ``input_name`` is not a declared graph input.
        """
        endpoints = self._flat.inputs[input_name]
        envelope = Envelope(message, self._control.now())
        for endpoint in endpoints:
            transport = self._inbox[endpoint]
            transport.put(envelope)
            self._control._record_enqueue(transport)

    def inject_payload(self, input_name: str, payload: Any, timestamp: int) -> None:
        """Inject ``payload`` at sample time ``timestamp``.

        A convenience for :meth:`inject`.
        """
        self.inject(input_name, Message(payload, timestamp))

    def poll(self, output_name: str) -> tuple[Message[Any], ...]:
        """Drain and return everything waiting on a graph output.

        Raises:
            KeyError: If ``output_name`` is not a declared graph output.
        """
        sink = self._sinks[output_name]
        drained: list[Message[Any]] = []
        while len(sink) > 0:
            envelope = sink.get()
            self._control._record_dequeue(sink, envelope)
            drained.append(envelope.message)
        return tuple(drained)

    def poll_all(self) -> dict[str, tuple[Message[Any], ...]]:
        """Drain every graph output, in declaration order."""
        return {name: self.poll(name) for name in self._sinks}

    def on_output(self, output_name: str, callback: MessageCallback) -> None:
        """Call ``callback(output_name, message)`` as each output message appears.

        Polling and callbacks are fed from the same publish step, so they can never
        disagree about what a run produced. A callback does not consume: a message
        is still there to :meth:`poll`.

        Raises:
            KeyError: If ``output_name`` is not a declared graph output.
        """
        if output_name not in self._sinks:
            raise KeyError(output_name)
        self._output_callbacks.setdefault(output_name, []).append(callback)

    def subscribe(self, topic: str, callback: MessageCallback) -> Subscription:
        """Tap an output port by topic *or* by alias.

        Subscribing to an alias and subscribing to the aliased topic observe the
        same output port and the same messages.

        Raises:
            KeyError: If ``topic`` names neither a topic nor an alias. The message
                lists what is available.
        """
        endpoint = self._flat.resolve_topic(topic)
        if endpoint is None:
            raise KeyError(
                f"{topic!r} is neither a topic nor an alias of this graph; "
                f"topics are {list(self.topics)} and aliases are {list(self.aliases)}"
            )
        subscription = Subscription(self, topic, endpoint, callback)
        self._taps.setdefault(endpoint, []).append(subscription)
        return subscription

    def subscribe_errors(self, qid: str, callback: MessageCallback) -> Subscription:
        """Tap the error topic of a node whose policy is ``route``.

        The topic is ``"<qualified node ID>.__error__"``, and it carries
        :class:`~dfg.scheduler.ErrorEvent` payloads. It has no edges: an error is
        observed, never routed into another node's input port.

        Raises:
            KeyError: If ``qid`` is not a node of this graph.
        """
        if qid not in self._flat.nodes:
            raise KeyError(qid)
        endpoint = (qid, ERROR_PORT)
        subscription = Subscription(self, topic_of(qid, ERROR_PORT), endpoint, callback)
        self._taps.setdefault(endpoint, []).append(subscription)
        return subscription

    def _remove_tap(self, subscription: Subscription) -> None:
        taps = self._taps.get(subscription.endpoint)
        if taps and subscription in taps:
            taps.remove(subscription)

    # --- Publishing ----------------------------------------------------------

    def _publish(
        self, qid: str, outputs: Mapping[str, tuple[Message[Any], ...]]
    ) -> None:
        """Move a firing's outputs onto taps, edges, and output sinks.

        The only place messages leave a node, and the order is fixed so that two
        runs of the same recording cannot differ:

        1. topic taps for the port -- each message once, however many nodes consume
           it, because the tap is at the port and not at the edge;
        2. outgoing edges, sorted by destination;
        3. any root-output sink, then that output's callbacks.

        ``outputs`` already arrives in *declared* port order, so the iteration order
        of the mapping a node returned is never trusted.
        """
        now = self._control.now()
        for port_name, messages in outputs.items():
            endpoint = (qid, port_name)
            for message in messages:
                self._fire_taps(endpoint, message)
            for transport in self._outgoing.get(endpoint, ()):
                for message in messages:
                    envelope = Envelope(message, now)
                    transport.put(envelope)
                    self._control._record_enqueue(transport)
            for output_name in self._output_sinks.get(endpoint, ()):
                sink = self._sinks[output_name]
                for message in messages:
                    envelope = Envelope(message, now)
                    sink.put(envelope)
                    self._control._record_enqueue(sink)
                    for callback in self._output_callbacks.get(output_name, ()):
                        callback(output_name, message)

    def _publish_error(self, qid: str, message: Message[ErrorEvent]) -> None:
        """Publish an error event on a node's error topic. Taps only."""
        self._fire_taps((qid, ERROR_PORT), message)

    def _fire_taps(self, endpoint: Endpoint, message: Message[Any]) -> None:
        for subscription in tuple(self._taps.get(endpoint, ())):
            if not subscription.cancelled:
                subscription.callback(subscription.name, message)

    # --- Introspection -------------------------------------------------------

    @property
    def control(self) -> ControlPlane:
        """The control plane for this graph."""
        return self._control

    @property
    def spec(self) -> GraphSpec:
        """The blueprint this was built from. Unchanged, and unchangeable."""
        return self._spec

    @property
    def flat(self) -> FlatGraph:
        """The flattened graph, for introspection and rendering."""
        return self._flat

    @property
    def nodes(self) -> Mapping[str, Node]:
        """Qualified ID to node instance."""
        return MappingProxyType(self._nodes)

    @property
    def topics(self) -> tuple[str, ...]:
        """Every topic, sorted. One per output port."""
        return tuple(sorted(self._flat.topics))

    @property
    def aliases(self) -> tuple[str, ...]:
        """Every graph-output alias at every scope, sorted."""
        return tuple(sorted(self._flat.aliases))

    @property
    def input_names(self) -> tuple[str, ...]:
        """Graph input names, in declaration order."""
        return tuple(self._flat.inputs)

    @property
    def output_names(self) -> tuple[str, ...]:
        """Graph output names, in declaration order."""
        return tuple(self._sinks)

    @property
    def order(self) -> tuple[str, ...]:
        """The canonical topological order of the qualified node IDs."""
        return self._order

    def __repr__(self) -> str:
        return (
            f"Graph({self._spec.name!r}, {len(self._nodes)} nodes, "
            f"{len(self._flat.edges)} edges, ordering={self._ordering!r})"
        )


def replay(
    spec: GraphSpec,
    registry: Registry,
    recording: Sequence[tuple[str, Message[Any]]],
    *,
    ordering: Ordering | str | None = None,
    clock: Clock = default_clock,
) -> dict[str, tuple[Message[Any], ...]]:
    """Run a recorded input sequence through a fresh instance and return its outputs.

    This is the offline-reprocessing path, and the reason determinism is a
    requirement rather than a nicety: the same blueprint plus the same injected
    sequence must produce the same outputs, so calling this twice must give equal
    results.

    Args:
        spec: The blueprint.
        registry: Resolves node types.
        recording: ``(input name, message)`` pairs, in injection order.
        ordering: As for :meth:`Graph.instantiate`.
        clock: As for :meth:`Graph.instantiate`. Does not affect outputs.

    Returns:
        Output name to the messages that output produced.
    """
    collected: dict[str, list[Message[Any]]] = {}
    with Graph.instantiate(spec, registry, ordering=ordering, clock=clock) as graph:
        for name in graph.output_names:
            collected[name] = []
        for input_name, message in recording:
            graph.inject(input_name, message)
            graph.run_until_idle()
            for name in graph.output_names:
                collected[name].extend(graph.poll(name))
    return {name: tuple(messages) for name, messages in collected.items()}


def iter_messages(
    outputs: Mapping[str, Sequence[Message[Any]]],
) -> Iterator[tuple[str, Message[Any]]]:
    """Flatten a poll result into ``(output name, message)`` pairs."""
    for name, messages in outputs.items():
        for message in messages:
            yield name, message


__all__ = [
    "Graph",
    "MessageCallback",
    "Subscription",
    "iter_messages",
    "replay",
]
