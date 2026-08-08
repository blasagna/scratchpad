"""Blueprint validation, before any node is constructed.

The blueprint layer is supposed to be cheap, and catching these problems after
instantiation would mean running every ``setup`` first. So everything here works
from the registry's descriptors rather than from instances.

:func:`validate` **aggregates**: it reports every problem it can find, not the
first. A half-built graph usually has several, and fixing them one run at a time is
a poor way to spend an afternoon. Structural checks run first; the cycle check runs
last, on the flattened graph, because that is the only place the question is exact.

One check is deliberately *not* here: whether an edge's transport name resolves.
That needs the transport registry, which is runtime, and this layer does not import
the runtime. :meth:`dfg.graph.Graph.instantiate` raises for an unknown transport.
"""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from typing import Any

from dfg.blueprint import (
    ERROR_POLICIES,
    OVERFLOW_POLICIES,
    AnyNodeSpec,
    GraphSpec,
    NodeSpec,
    ParamRef,
    PortRef,
    SubgraphSpec,
)
from dfg.errors import Problem, ValidationError
from dfg.flatten import find_cycle, flatten
from dfg.node import _Required
from dfg.ports import PortSpec, is_reserved_name, is_valid_name
from dfg.readiness import AllInputs, PredicateRule, ReadinessRule, is_registered_kind
from dfg.registry import Registry

__all__ = ["Problem", "check", "validate"]


def validate(spec: GraphSpec, registry: Registry) -> None:
    """Check a blueprint, raising with *every* problem found.

    Raises:
        ValidationError: If anything is wrong. ``error.problems`` holds them all.
    """
    problems = check(spec, registry)
    if problems:
        raise ValidationError(problems)


def check(spec: GraphSpec, registry: Registry) -> tuple[Problem, ...]:
    """Return every problem with a blueprint, or an empty tuple if it is valid."""
    problems = list(_check_scope(spec, registry, scope=()))
    if problems:
        # Flattening needs the structure to hold together, so a cycle check on a
        # blueprint with dangling edges would be reporting on a graph that does
        # not exist. Report what we have and let the author come back.
        return tuple(problems)
    cycle = find_cycle(flatten(spec, registry))
    if cycle is not None:
        problems.append(
            Problem("cycle", f"the graph is not acyclic: {' -> '.join(cycle)}", ())
        )
    return tuple(problems)


def _check_scope(
    graph: GraphSpec, registry: Registry, *, scope: tuple[str, ...]
) -> Iterator[Problem]:
    """Check one graph and recurse into its subgraphs."""
    yield from _check_unique_ids(graph, scope)
    yield from _check_names(graph, scope)

    # Port tables per child, so edge and boundary checks can look them up. A child
    # whose type is unknown gets no table, and is skipped by later checks -- one
    # problem per cause reads better than a cascade.
    inputs: dict[str, dict[str, PortSpec]] = {}
    outputs: dict[str, dict[str, PortSpec]] = {}

    for child in graph.nodes:
        if isinstance(child, SubgraphSpec):
            yield from _check_scope(child.graph, registry, scope=(*scope, child.id))
            inputs[child.id] = {
                boundary.name: PortSpec(boundary.name, boundary.type_tag)
                for boundary in child.graph.inputs
            }
            outputs[child.id] = {
                boundary.name: PortSpec(boundary.name, boundary.type_tag)
                for boundary in child.graph.outputs
            }
            yield from _check_params(
                child.params, dict.fromkeys(child.graph.params), graph, child, scope
            )
            continue

        info = registry.describe(child.type)
        if info is None:
            yield Problem(
                "unknown_type",
                f"node {child.id!r} has type {child.type!r}, which is not "
                f"registered; registered types are {list(registry.names())}",
                scope,
            )
            continue
        yield from _check_params(child.params, info.params, graph, child, scope)
        yield from _check_readiness(child, scope)
        yield from _check_policies(child, scope)

        resolved = {name: None for name in info.params} | dict(child.params)
        node_inputs = info.input_ports(resolved)
        node_outputs = info.output_ports(resolved)
        yield from _check_port_names(child.id, node_inputs, "input", scope)
        yield from _check_port_names(child.id, node_outputs, "output", scope)
        if not node_inputs:
            yield Problem(
                "source_node",
                f"node {child.id!r} of type {child.type!r} has no input ports, so "
                f"no input-driven readiness rule can ever fire it; graph inputs "
                f"are the only sources",
                scope,
            )
        inputs[child.id] = {port.name: port for port in node_inputs}
        outputs[child.id] = {port.name: port for port in node_outputs}

    yield from _check_edges(graph, inputs, outputs, scope)
    yield from _check_boundaries(graph, inputs, outputs, scope)
    yield from _check_writers(graph, inputs, scope)
    yield from _check_unfireable(graph, inputs, scope)


def _check_unique_ids(graph: GraphSpec, scope: tuple[str, ...]) -> Iterator[Problem]:
    for label, names in (
        ("node", [child.id for child in graph.nodes]),
        ("input", [boundary.name for boundary in graph.inputs]),
        ("output", [boundary.name for boundary in graph.outputs]),
    ):
        seen: set[str] = set()
        for name in names:
            if name in seen:
                yield Problem(
                    "duplicate_id",
                    f"{label} name {name!r} is declared more than once; names must "
                    f"be unique among their siblings",
                    scope,
                )
            seen.add(name)


def _check_names(graph: GraphSpec, scope: tuple[str, ...]) -> Iterator[Problem]:
    for child in graph.nodes:
        if "." in child.id:
            yield Problem(
                "bad_name",
                f"node ID {child.id!r} contains a dot; qualified IDs and topics "
                f"are dotted paths, so a node ID may not be",
                scope,
            )
        elif not is_valid_name(child.id):
            yield Problem(
                "bad_name",
                f"node ID {child.id!r} must start with a letter and contain only "
                f"letters, digits, and underscores",
                scope,
            )
    for label, names in (
        ("input", [b.name for b in graph.inputs]),
        ("output", [b.name for b in graph.outputs]),
    ):
        for name in names:
            if not is_valid_name(name):
                yield Problem(
                    "bad_name",
                    f"graph {label} name {name!r} is not a legal name",
                    scope,
                )


def _check_port_names(
    node_id: str, ports: tuple[PortSpec, ...], kind: str, scope: tuple[str, ...]
) -> Iterator[Problem]:
    seen: set[str] = set()
    for port in ports:
        if port.name in seen:
            yield Problem(
                "duplicate_id",
                f"node {node_id!r} declares {kind} port {port.name!r} twice",
                scope,
            )
        seen.add(port.name)
        if is_reserved_name(port.name):
            yield Problem(
                "reserved_name",
                f"node {node_id!r} declares {kind} port {port.name!r}; the '__' "
                f"prefix is reserved for framework ports",
                scope,
            )
        elif not is_valid_name(port.name):
            yield Problem(
                "bad_name",
                f"node {node_id!r} {kind} port {port.name!r} is not a legal name",
                scope,
            )


def _check_params(
    given: Mapping[str, Any],
    declared: Mapping[str, Any],
    graph: GraphSpec,
    child: AnyNodeSpec,
    scope: tuple[str, ...],
) -> Iterator[Problem]:
    kind = "subgraph" if isinstance(child, SubgraphSpec) else "node"
    for name, value in given.items():
        if name not in declared:
            yield Problem(
                "unknown_param",
                f"{kind} {child.id!r} sets parameter {name!r}, which it does not "
                f"declare; it declares {sorted(declared)}",
                scope,
            )
        if isinstance(value, ParamRef) and value.name not in graph.params:
            yield Problem(
                "unresolved_param",
                f"{kind} {child.id!r} parameter {name!r} references graph "
                f"parameter {value.name!r}, which this graph does not declare; "
                f"it declares {sorted(graph.params)}",
                scope,
            )
    for name, default in declared.items():
        if isinstance(default, _Required) and name not in given:
            yield Problem(
                "missing_param",
                f"{kind} {child.id!r} does not set required parameter {name!r}",
                scope,
            )


def _check_readiness(child: NodeSpec, scope: tuple[str, ...]) -> Iterator[Problem]:
    rule = child.readiness
    if not isinstance(rule, ReadinessRule):
        yield Problem(
            "bad_readiness",
            f"node {child.id!r} readiness is {type(rule).__name__}, not a "
            f"ReadinessRule",
            scope,
        )
        return
    if isinstance(rule, PredicateRule):
        # Legal in memory, just not serializable. serialize.dumps is where that
        # becomes an error, so validation stays quiet about it.
        return
    if not is_registered_kind(rule.KIND):
        yield Problem(
            "unknown_readiness",
            f"node {child.id!r} uses readiness kind {rule.KIND!r}, which is not "
            f"registered, so this blueprint cannot round-trip",
            scope,
        )


def _check_policies(child: NodeSpec, scope: tuple[str, ...]) -> Iterator[Problem]:
    if child.on_error not in ERROR_POLICIES:
        yield Problem(
            "bad_policy",
            f"node {child.id!r} has error policy {child.on_error!r}; "
            f"choose one of {sorted(ERROR_POLICIES)}",
            scope,
        )


def _check_edges(
    graph: GraphSpec,
    inputs: Mapping[str, Mapping[str, PortSpec]],
    outputs: Mapping[str, Mapping[str, PortSpec]],
    scope: tuple[str, ...],
) -> Iterator[Problem]:
    for edge in graph.edges:
        src_port = _lookup(edge.src, outputs, "output", graph, scope)
        dst_port = _lookup(edge.dst, inputs, "input", graph, scope)
        for problem in (src_port, dst_port):
            if isinstance(problem, Problem):
                yield problem
        if isinstance(src_port, Problem) or isinstance(dst_port, Problem):
            continue
        if edge.on_overflow not in OVERFLOW_POLICIES:
            yield Problem(
                "bad_policy",
                f"edge {edge} has overflow policy {edge.on_overflow!r}; choose one "
                f"of {sorted(OVERFLOW_POLICIES)} -- 'block' deadlocks a "
                f"single-threaded scheduler and is deliberately absent",
                scope,
            )
        # An edge's transport name is *not* checked here: resolving it needs the
        # transport registry, which is runtime, and the blueprint layer does not
        # import the runtime. Graph.instantiate raises UnknownTransportError.
        yield from _check_tags(f"edge {edge}", src_port, dst_port, scope)


def _check_boundaries(
    graph: GraphSpec,
    inputs: Mapping[str, Mapping[str, PortSpec]],
    outputs: Mapping[str, Mapping[str, PortSpec]],
    scope: tuple[str, ...],
) -> Iterator[Problem]:
    for boundary in graph.inputs:
        if not boundary.targets:
            yield Problem(
                "dangling_boundary",
                f"graph input {boundary.name!r} has no targets, so nothing "
                f"injected into it could go anywhere",
                scope,
            )
        for target in boundary.targets:
            port = _lookup(target, inputs, "input", graph, scope)
            if isinstance(port, Problem):
                yield port
                continue
            yield from _check_tags(
                f"graph input {boundary.name!r} -> {target}",
                PortSpec(boundary.name, boundary.type_tag),
                port,
                scope,
            )
    for boundary in graph.outputs:
        port = _lookup(boundary.source, outputs, "output", graph, scope)
        if isinstance(port, Problem):
            yield port
            continue
        yield from _check_tags(
            f"graph output {boundary.name!r} -> {boundary.source}",
            port,
            PortSpec(boundary.name, boundary.type_tag),
            scope,
        )


def _check_writers(
    graph: GraphSpec,
    inputs: Mapping[str, Mapping[str, PortSpec]],
    scope: tuple[str, ...],
) -> Iterator[Problem]:
    """One writer per input port.

    Two producers into one queue would make message order depend on which of them
    the scheduler happened to fire first, and determinism cannot tolerate that. A
    merge node with N input ports says the same thing explicitly.

    Checking per scope is complete, which is not obvious: every writer of an input
    port is declared in the *same* graph as the node owning that port -- either an
    edge in that graph or that graph's own input boundary. A parent edge into a
    subgraph resolves to the subgraph's boundary declaration, so it is that
    subgraph's targets, counted here when this graph is the subgraph.
    """
    writers: dict[PortRef, list[str]] = {}
    for edge in graph.edges:
        writers.setdefault(edge.dst, []).append(f"edge from {edge.src}")
    for boundary in graph.inputs:
        for target in boundary.targets:
            writers.setdefault(target, []).append(f"graph input {boundary.name!r}")
    for ref, sources in writers.items():
        if len(sources) > 1 and ref.node in inputs:
            yield Problem(
                "fan_in",
                f"input port {ref} has {len(sources)} writers "
                f"({', '.join(sorted(sources))}); an input port takes one writer, "
                f"so use a merge node with one port per producer",
                scope,
            )


def _check_unfireable(
    graph: GraphSpec,
    inputs: Mapping[str, Mapping[str, PortSpec]],
    scope: tuple[str, ...],
) -> Iterator[Problem]:
    """A node whose readiness rule can provably never be satisfied."""
    written: set[PortRef] = {edge.dst for edge in graph.edges}
    for boundary in graph.inputs:
        written.update(boundary.targets)
    for child in graph.nodes:
        if isinstance(child, SubgraphSpec) or child.id not in inputs:
            continue
        if not isinstance(child.readiness, AllInputs):
            continue
        unwired = sorted(
            name for name in inputs[child.id] if PortRef(child.id, name) not in written
        )
        if unwired:
            yield Problem(
                "never_ready",
                f"node {child.id!r} has 'all' readiness but input port(s) "
                f"{unwired} have no writer, so it can never fire",
                scope,
            )


def _lookup(
    ref: PortRef,
    tables: Mapping[str, Mapping[str, PortSpec]],
    kind: str,
    graph: GraphSpec,
    scope: tuple[str, ...],
) -> PortSpec | Problem:
    """Resolve a port reference against the per-child port tables."""
    if ref.node not in tables:
        if graph.node(ref.node) is None:
            return Problem(
                "dangling_edge",
                f"{ref} names node {ref.node!r}, which this graph does not "
                f"declare; it declares {[child.id for child in graph.nodes]}",
                scope,
            )
        # The node exists but has no port table, because its type was unknown.
        # That problem is already reported; do not pile on.
        return PortSpec(ref.port)
    ports = tables[ref.node]
    if ref.port not in ports:
        return Problem(
            "dangling_edge",
            f"{ref} names {kind} port {ref.port!r}, which node {ref.node!r} does "
            f"not declare; it declares {sorted(ports)}",
            scope,
        )
    return ports[ref.port]


def _check_tags(
    where: str, producer: PortSpec, consumer: PortSpec, scope: tuple[str, ...]
) -> Iterator[Problem]:
    """Compare type tags only where an author supplied both.

    Port type tags are optional and untyped is the default, so this is the one
    check that is allowed to be silent.
    """
    if producer.type_tag is None or consumer.type_tag is None:
        return
    if producer.type_tag != consumer.type_tag:
        yield Problem(
            "type_mismatch",
            f"{where} connects type tag {producer.type_tag!r} to {consumer.type_tag!r}",
            scope,
        )
