"""Flattening a nested blueprint into one node set keyed by qualified ID.

Nesting is a blueprint concept. At runtime there is one node set, one edge list,
one topological order, one ready set, and one place ties break -- because a
subgraph that stayed a runtime node would have to answer two questions the
contract does not: when is *it* ready (``all`` over its boundary is stricter than
the flat equivalent), and how much of itself runs per firing (anything but "to
quiescence" reorders firings relative to flat; "to quiescence" starves the
parent's other ready nodes). Two execution modes that disagree would reduce
determinism to "deterministic per mode".

What that costs, plainly:

* **Boundary ports vanish.** A parent edge into a subgraph input with two targets
  becomes two flat edges, so a ``capacity=64`` parent edge becomes two independent
  64-deep queues rather than one shared one, and there is no single queue to report
  as "the subgraph's input depth".
* **Graph parameters must resolve here**, since no runtime object holds a
  subgraph's parameters. That is what :class:`~dfg.blueprint.ParamRef` is for.
* **A qualified ID must map back to its declaring scope** for error messages, which
  is why :attr:`FlatNode.scope` is carried.
* **No per-subgraph start/stop**, ever, without replacing the runtime. If that
  becomes a requirement, this module and the blueprint layer survive it.
"""

from __future__ import annotations

import heapq
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

from dfg.blueprint import (
    ErrorPolicy,
    GraphSpec,
    NodeSpec,
    Overflow,
    ParamRef,
    PortRef,
    SubgraphSpec,
)
from dfg.errors import Problem, ValidationError
from dfg.ports import PortSpec, qualify, topic_of
from dfg.readiness import ReadinessRule
from dfg.registry import Registry

type Endpoint = tuple[str, str]
"""A resolved ``(qualified node ID, port name)`` pair."""


@dataclass(frozen=True, slots=True)
class FlatNode:
    """A leaf node with its identity and policies resolved.

    Attributes:
        qid: The qualified node ID -- enclosing subgraph IDs and its own, dotted.
        scope: The enclosing subgraph IDs, for messages that name the right graph.
        type_name: The registered type name.
        params: Parameters with every :class:`~dfg.blueprint.ParamRef` resolved.
        readiness: When this node may fire.
        on_error: What happens when its ``run`` raises.
        priority: For the ``priority`` ordering.
        inputs: Declared input ports, in declaration order.
        outputs: Declared output ports, in declaration order. Publishing walks
            these in this order.
    """

    qid: str
    scope: tuple[str, ...]
    type_name: str
    params: Mapping[str, Any]
    readiness: ReadinessRule
    on_error: ErrorPolicy | str
    priority: int
    inputs: tuple[PortSpec, ...]
    outputs: tuple[PortSpec, ...]

    def input_port(self, name: str) -> PortSpec | None:
        """The declared input port named ``name``, or ``None``."""
        return next((port for port in self.inputs if port.name == name), None)

    def output_port(self, name: str) -> PortSpec | None:
        """The declared output port named ``name``, or ``None``."""
        return next((port for port in self.outputs if port.name == name), None)


@dataclass(frozen=True, slots=True)
class FlatEdge:
    """One resolved connection between two qualified ports.

    Attributes:
        src: The producing ``(qid, port)``.
        dst: The consuming ``(qid, port)``. Unique across a flat graph -- one
            writer per input port.
        capacity: ``None`` for unbounded.
        on_overflow: What a full bounded edge does.
        transport: The registered transport name.
        declared_in: The scope of the graph whose edge (or boundary declaration)
            produced this, for error messages.
    """

    src: Endpoint
    dst: Endpoint
    capacity: int | None
    on_overflow: Overflow | str
    transport: str
    declared_in: tuple[str, ...] = ()

    @property
    def key(self) -> str:
        """A stable human-readable identifier, used by the control plane."""
        return f"{self.src[0]}.{self.src[1]} -> {self.dst[0]}.{self.dst[1]}"


@dataclass(frozen=True, slots=True)
class FlatGraph:
    """A whole blueprint resolved into one runnable shape.

    Attributes:
        name: The root graph's name.
        nodes: Qualified ID to node. Insertion order is declaration order,
            outer-first.
        edges: Every resolved connection.
        inputs: Root input name to the ``(qid, port)`` endpoints it feeds.
        outputs: Root output name to the single ``(qid, port)`` it aliases.
        topics: Every output port's topic, ``"<qid>.<port>"``, to its endpoint.
        aliases: Every graph-output name at every scope to the endpoint it aliases
            -- ``"classify.classified"`` and the root's ``"result"``. An alias is not a
            separate topic; subscribing to it observes the aliased port.
    """

    name: str
    nodes: Mapping[str, FlatNode]
    edges: tuple[FlatEdge, ...]
    inputs: Mapping[str, tuple[Endpoint, ...]]
    outputs: Mapping[str, Endpoint]
    topics: Mapping[str, Endpoint]
    aliases: Mapping[str, Endpoint]

    def resolve_topic(self, name: str) -> Endpoint | None:
        """Resolve a topic *or* an alias to the output port it names."""
        if name in self.topics:
            return self.topics[name]
        return self.aliases.get(name)

    def writer_of(self, endpoint: Endpoint) -> FlatEdge | None:
        """The edge feeding an input port endpoint, if any."""
        return next((edge for edge in self.edges if edge.dst == endpoint), None)

    def edges_from(self, endpoint: Endpoint) -> tuple[FlatEdge, ...]:
        """Every edge leaving an output port endpoint."""
        return tuple(edge for edge in self.edges if edge.src == endpoint)


def flatten(spec: GraphSpec, registry: Registry) -> FlatGraph:
    """Resolve a possibly nested blueprint into a :class:`FlatGraph`.

    Args:
        spec: The root graph.
        registry: Resolves type names to the port and parameter descriptors. No
            node is constructed.

    Returns:
        The flattened graph.

    Raises:
        ValidationError: On a structural problem that makes flattening impossible
            (an unknown type, an edge naming a missing node or boundary, an
            unresolvable parameter reference). :func:`dfg.validate.validate`
            reports these -- and everything else -- as a set rather than one at a
            time, so call it first for a good message.
    """
    state = _FlattenState(registry)
    inputs, outputs = state.scope(spec, scope=(), params=dict(spec.params))
    for name, endpoint in outputs.items():
        state.aliases[name] = endpoint
    return FlatGraph(
        name=spec.name,
        nodes=state.nodes,
        edges=tuple(state.edges),
        inputs=inputs,
        outputs=outputs,
        topics=state.topics,
        aliases=state.aliases,
    )


class _FlattenState:
    """Accumulates the flat graph while recursing through nested scopes."""

    def __init__(self, registry: Registry) -> None:
        self.registry = registry
        self.nodes: dict[str, FlatNode] = {}
        self.edges: list[FlatEdge] = []
        self.topics: dict[str, Endpoint] = {}
        self.aliases: dict[str, Endpoint] = {}

    def scope(
        self,
        graph: GraphSpec,
        *,
        scope: tuple[str, ...],
        params: Mapping[str, Any],
    ) -> tuple[dict[str, tuple[Endpoint, ...]], dict[str, Endpoint]]:
        """Flatten one graph, returning its resolved boundary maps.

        The maps are what the *parent* needs: an input name resolves to the inner
        endpoints it feeds (possibly several, possibly reached through further
        nesting), and an output name resolves to the single endpoint it aliases.
        """
        sub_inputs: dict[str, dict[str, tuple[Endpoint, ...]]] = {}
        sub_outputs: dict[str, dict[str, Endpoint]] = {}

        for child in graph.nodes:
            if isinstance(child, SubgraphSpec):
                child_scope = (*scope, child.node_id)
                overrides = {
                    name: _resolve_param(value, params, scope, child.node_id)
                    for name, value in child.params.items()
                }
                child_params = {**child.graph.params, **overrides}
                inner_inputs, inner_outputs = self.scope(
                    child.graph, scope=child_scope, params=child_params
                )
                sub_inputs[child.node_id] = inner_inputs
                sub_outputs[child.node_id] = inner_outputs
                prefix = qualify(scope, child.node_id)
                for name, endpoint in inner_outputs.items():
                    self.aliases[f"{prefix}.{name}"] = endpoint
            else:
                self._leaf(child, scope=scope, params=params)

        for edge in graph.edges:
            src = self._resolve_src(edge.src, scope, sub_outputs, graph)
            for dst in self._resolve_dst(edge.dst, scope, sub_inputs, graph):
                self.edges.append(
                    FlatEdge(
                        src=src,
                        dst=dst,
                        capacity=edge.capacity,
                        on_overflow=edge.on_overflow,
                        transport=edge.transport,
                        declared_in=scope,
                    )
                )

        inputs = {
            boundary.name: tuple(
                endpoint
                for target in boundary.targets
                for endpoint in self._resolve_dst(target, scope, sub_inputs, graph)
            )
            for boundary in graph.inputs
        }
        outputs = {
            boundary.name: self._resolve_src(boundary.source, scope, sub_outputs, graph)
            for boundary in graph.outputs
        }
        return inputs, outputs

    def _leaf(
        self, spec: NodeSpec, *, scope: tuple[str, ...], params: Mapping[str, Any]
    ) -> None:
        qid = qualify(scope, spec.node_id)
        info = self.registry.describe(spec.type_name)
        if info is None:
            raise ValidationError(
                (
                    Problem(
                        "unknown_type",
                        f"node {spec.node_id!r} has unregistered type {spec.type_name!r}",
                        scope,
                    ),
                )
            )
        resolved = {
            name: _resolve_param(value, params, scope, spec.node_id)
            for name, value in spec.params.items()
        }
        node = FlatNode(
            qid=qid,
            scope=scope,
            type_name=spec.type_name,
            params=resolved,
            readiness=spec.readiness,
            on_error=spec.on_error,
            priority=spec.priority,
            inputs=info.input_ports(resolved),
            outputs=info.output_ports(resolved),
        )
        self.nodes[qid] = node
        for port in node.outputs:
            self.topics[topic_of(qid, port.name)] = (qid, port.name)

    def _resolve_src(
        self,
        ref: PortRef,
        scope: tuple[str, ...],
        sub_outputs: Mapping[str, Mapping[str, Endpoint]],
        graph: GraphSpec,
    ) -> Endpoint:
        """Resolve a producing end, following an alias through any nesting."""
        if ref.node in sub_outputs:
            inner = sub_outputs[ref.node]
            if ref.port not in inner:
                raise ValidationError(
                    (
                        Problem(
                            "dangling_edge",
                            f"subgraph {ref.node!r} has no output named "
                            f"{ref.port!r}; it declares {sorted(inner)}",
                            scope,
                        ),
                    )
                )
            return inner[ref.port]
        self._require_leaf(ref, scope, graph)
        return (qualify(scope, ref.node), ref.port)

    def _resolve_dst(
        self,
        ref: PortRef,
        scope: tuple[str, ...],
        sub_inputs: Mapping[str, Mapping[str, tuple[Endpoint, ...]]],
        graph: GraphSpec,
    ) -> tuple[Endpoint, ...]:
        """Resolve a consuming end, expanding a subgraph input's fan-out."""
        if ref.node in sub_inputs:
            inner = sub_inputs[ref.node]
            if ref.port not in inner:
                raise ValidationError(
                    (
                        Problem(
                            "dangling_edge",
                            f"subgraph {ref.node!r} has no input named "
                            f"{ref.port!r}; it declares {sorted(inner)}",
                            scope,
                        ),
                    )
                )
            return inner[ref.port]
        self._require_leaf(ref, scope, graph)
        return ((qualify(scope, ref.node), ref.port),)

    def _require_leaf(
        self, ref: PortRef, scope: tuple[str, ...], graph: GraphSpec
    ) -> None:
        if graph.node(ref.node) is None:
            raise ValidationError(
                (
                    Problem(
                        "dangling_edge",
                        f"no node named {ref.node!r} in this graph; "
                        f"it declares {[n.node_id for n in graph.nodes]}",
                        scope,
                    ),
                )
            )


def _resolve_param(
    value: Any, params: Mapping[str, Any], scope: tuple[str, ...], node_id: str
) -> Any:
    """Resolve a :class:`~dfg.blueprint.ParamRef` against the graph's parameters."""
    if not isinstance(value, ParamRef):
        return value
    if value.name not in params:
        raise ValidationError(
            (
                Problem(
                    "unresolved_param",
                    f"node {node_id!r} references graph parameter "
                    f"{value.name!r}, which this graph does not declare; "
                    f"it declares {sorted(params)}",
                    scope,
                ),
            )
        )
    return params[value.name]


# --- Graph algorithms over a flat graph --------------------------------------


def predecessors(flat: FlatGraph) -> dict[str, set[str]]:
    """Qualified ID to the set of qualified IDs that feed it."""
    preds: dict[str, set[str]] = {qid: set() for qid in flat.nodes}
    for edge in flat.edges:
        src_qid, dst_qid = edge.src[0], edge.dst[0]
        if src_qid in preds and dst_qid in preds:
            preds[dst_qid].add(src_qid)
    return preds


def successors(flat: FlatGraph) -> dict[str, set[str]]:
    """Qualified ID to the set of qualified IDs it feeds."""
    succs: dict[str, set[str]] = {qid: set() for qid in flat.nodes}
    for edge in flat.edges:
        src_qid, dst_qid = edge.src[0], edge.dst[0]
        if src_qid in succs and dst_qid in succs:
            succs[src_qid].add(dst_qid)
    return succs


def find_cycle(flat: FlatGraph) -> tuple[str, ...] | None:
    """Return one cycle as a tuple of qualified IDs, or ``None`` if acyclic.

    Cycles are checked *here*, on the flattened graph, rather than by treating a
    subgraph as a single vertex -- that would report a cycle between two subgraphs
    whose inner wiring does not actually form one.
    """
    succs = successors(flat)
    WHITE, GREY, BLACK = 0, 1, 2
    colour = dict.fromkeys(flat.nodes, WHITE)
    stack: list[str] = []

    def visit(qid: str) -> tuple[str, ...] | None:
        colour[qid] = GREY
        stack.append(qid)
        for nxt in sorted(succs[qid]):
            if colour[nxt] == GREY:
                return tuple(stack[stack.index(nxt) :]) + (nxt,)
            if colour[nxt] == WHITE:
                found = visit(nxt)
                if found is not None:
                    return found
        stack.pop()
        colour[qid] = BLACK
        return None

    for qid in sorted(flat.nodes):
        if colour[qid] == WHITE:
            found = visit(qid)
            if found is not None:
                return found
    return None


def topological_order(flat: FlatGraph) -> tuple[str, ...]:
    """Return the canonical topological order of the qualified IDs.

    A topological order is not unique, so this one is made unique: Kahn's
    algorithm with a min-heap keyed by qualified ID. That is what lets `setup`,
    `teardown`, and the ``topological`` ordering all be reproducible across runs
    and across declaration-order changes.

    Raises:
        ValidationError: If the graph has a cycle.
    """
    preds = predecessors(flat)
    remaining = {qid: len(parents) for qid, parents in preds.items()}
    succs = successors(flat)
    ready = [qid for qid, count in remaining.items() if count == 0]
    heapq.heapify(ready)
    order: list[str] = []
    while ready:
        qid = heapq.heappop(ready)
        order.append(qid)
        for nxt in sorted(succs[qid]):
            remaining[nxt] -= 1
            if remaining[nxt] == 0:
                heapq.heappush(ready, nxt)
    if len(order) != len(flat.nodes):
        cycle = find_cycle(flat)
        detail = " -> ".join(cycle) if cycle else "unknown"
        raise ValidationError(
            (Problem("cycle", f"the graph is not acyclic: {detail}", ()),)
        )
    return tuple(order)


def levels(flat: FlatGraph, order: tuple[str, ...] | None = None) -> dict[str, int]:
    """Longest-path level per node: ``0`` for nodes fed only by graph inputs.

    Longest path, not shortest: with ``1 + max(pred)`` a node is never at a lower
    level than something feeding it, so a level-ordered run cannot fire a node
    ahead of its own predecessor. Shortest-path levels would allow exactly that.

    Args:
        order: A precomputed ``topological_order(flat)``, for a caller that
            already has one. Computed fresh when omitted.

    Raises:
        ValidationError: If the graph has a cycle.
    """
    preds = predecessors(flat)
    result: dict[str, int] = {}
    for qid in order if order is not None else topological_order(flat):
        parents = preds[qid]
        result[qid] = 1 + max((result[p] for p in parents), default=-1)
    return result
