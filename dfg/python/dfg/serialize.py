"""Blueprint serialization, in stdlib JSON.

JSON because the core is stdlib-only, because it diffs readably in git, and
because every future port has a reader for it. The alternatives and why not:
YAML is a dependency in what is supposed to be the portable core; ``tomllib`` is
read-only, so a TOML round-trip needs a hand-written emitter; and pickle is
unportable, unsafe, and would let a blueprint smuggle live objects across the
layer the contract draws.

:func:`from_dict` needs **no registry**. It happily produces a blueprint whose
type names nothing can resolve -- which is the point of having a registry as a
concept: :meth:`dfg.graph.Graph.instantiate` is where an unknown type becomes an
error.
"""

from __future__ import annotations

import json
from collections.abc import Mapping
from enum import StrEnum
from typing import Any

from dfg.blueprint import (
    AnyNodeSpec,
    EdgeSpec,
    EdgeTransport,
    ErrorPolicy,
    GraphInput,
    GraphOutput,
    GraphSpec,
    NodeSpec,
    Overflow,
    ParamRef,
    PortRef,
    SubgraphSpec,
)
from dfg.errors import SchemaVersionError, SerializationError
from dfg.readiness import ReadinessRule, readiness_from_dict

SCHEMA_VERSION = 1
"""Bumped when the on-disk shape changes incompatibly."""

PARAM_REF_KEY = "$param"
"""How a :class:`~dfg.blueprint.ParamRef` spells itself in JSON."""


def to_dict(spec: GraphSpec) -> dict[str, Any]:
    """Convert a blueprint to a JSON-ready dict, with a schema version."""
    return {"schema_version": SCHEMA_VERSION, "graph": _graph_to_dict(spec)}


def from_dict(data: Mapping[str, Any]) -> GraphSpec:
    """Rebuild a blueprint from :func:`to_dict`'s output.

    Raises:
        SchemaVersionError: If the schema version is missing or unreadable.
        SerializationError: If the structure is not a serialized blueprint.
    """
    version = data.get("schema_version")
    if version is None:
        raise SchemaVersionError(
            f"no 'schema_version' key; this port writes version {SCHEMA_VERSION}"
        )
    if version != SCHEMA_VERSION:
        raise SchemaVersionError(
            f"schema version {version!r} cannot be read by this port, which "
            f"writes version {SCHEMA_VERSION}"
        )
    graph = data.get("graph")
    if not isinstance(graph, Mapping):
        raise SerializationError("no 'graph' object to read")
    return _graph_from_dict(graph)


def dumps(spec: GraphSpec, *, indent: int | None = 2) -> str:
    """Serialize a blueprint to JSON text.

    Key order is the declaration order the writers below use, never sorted, so a
    round-trip is byte-stable and a diff shows what actually changed.

    Raises:
        SerializationError: If a parameter value or readiness rule cannot be
            written. The message names the node.
    """
    return json.dumps(to_dict(spec), indent=indent, sort_keys=False)


def loads(text: str) -> GraphSpec:
    """Deserialize a blueprint from JSON text.

    Raises:
        SchemaVersionError: If the schema version is missing or unreadable.
        SerializationError: If the text is not JSON, or not a blueprint.
    """
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SerializationError(f"not valid JSON: {exc}") from exc
    if not isinstance(data, Mapping):
        raise SerializationError(f"expected a JSON object, got {type(data).__name__}")
    return from_dict(data)


# --- Writing -----------------------------------------------------------------


def _graph_to_dict(spec: GraphSpec) -> dict[str, Any]:
    return {
        "name": spec.name,
        "params": _params_to_dict(spec.params, where=f"graph {spec.name!r}"),
        "inputs": [_input_to_dict(b) for b in spec.inputs],
        "outputs": [_output_to_dict(b) for b in spec.outputs],
        "nodes": [_node_to_dict(child) for child in spec.nodes],
        "edges": [_edge_to_dict(edge) for edge in spec.edges],
    }


def _node_to_dict(child: AnyNodeSpec) -> dict[str, Any]:
    if isinstance(child, SubgraphSpec):
        return {
            "kind": "subgraph",
            "id": child.node_id,
            "params": _params_to_dict(
                child.params, where=f"subgraph {child.node_id!r}"
            ),
            "graph": _graph_to_dict(child.graph),
        }
    return {
        "kind": "node",
        "id": child.node_id,
        "type": child.type_name,
        "params": _params_to_dict(child.params, where=f"node {child.node_id!r}"),
        "readiness": _readiness_to_dict(child.readiness, child.node_id),
        # str() so the dict holds JSON primitives, not enum members.
        "on_error": str(child.on_error),
        "priority": child.priority,
    }


def _edge_to_dict(edge: EdgeSpec) -> dict[str, Any]:
    return {
        "src": _ref_to_dict(edge.src),
        "dst": _ref_to_dict(edge.dst),
        "capacity": edge.capacity,
        "on_overflow": str(edge.on_overflow),
        "transport": str(edge.transport),
    }


def _input_to_dict(boundary: GraphInput) -> dict[str, Any]:
    return {
        "name": boundary.name,
        "type_tag": boundary.type_tag,
        "targets": [_ref_to_dict(ref) for ref in boundary.targets],
    }


def _output_to_dict(boundary: GraphOutput) -> dict[str, Any]:
    return {
        "name": boundary.name,
        "type_tag": boundary.type_tag,
        "source": _ref_to_dict(boundary.source),
    }


def _ref_to_dict(ref: PortRef) -> dict[str, str]:
    return {"node": ref.node, "port": ref.port}


def _readiness_to_dict(rule: ReadinessRule, node_id: str) -> dict[str, Any]:
    try:
        return rule.to_dict()
    except SerializationError as exc:
        raise SerializationError(f"node {node_id!r}: {exc}") from exc


def _params_to_dict(params: Mapping[str, Any], *, where: str) -> dict[str, Any]:
    return {name: _param_to_json(value, where, name) for name, value in params.items()}


def _param_to_json(value: Any, where: str, name: str) -> Any:
    """Convert one parameter value, rejecting anything JSON cannot carry.

    Parameters carry numbers, strings, and small containers. Payloads carry
    ndarrays and record batches -- those travel as messages, not as configuration,
    and a blueprint that embedded one would not be serializable at all.
    """
    if isinstance(value, ParamRef):
        return {PARAM_REF_KEY: value.name}
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Mapping):
        return {
            str(key): _param_to_json(item, where, f"{name}.{key}")
            for key, item in value.items()
        }
    if isinstance(value, (list, tuple)):
        return [
            _param_to_json(item, where, f"{name}[{i}]") for i, item in enumerate(value)
        ]
    raise SerializationError(
        f"{where} parameter {name!r} is a {type(value).__name__}, which JSON "
        f"cannot carry; parameters hold numbers, strings, and small containers"
    )


# --- Reading -----------------------------------------------------------------


def _graph_from_dict(data: Mapping[str, Any]) -> GraphSpec:
    return GraphSpec(
        name=_require(data, "name", str),
        nodes=tuple(_node_from_dict(item) for item in data.get("nodes", ())),
        edges=tuple(_edge_from_dict(item) for item in data.get("edges", ())),
        inputs=tuple(_input_from_dict(item) for item in data.get("inputs", ())),
        outputs=tuple(_output_from_dict(item) for item in data.get("outputs", ())),
        params=_params_from_dict(data.get("params", {})),
    )


def _node_from_dict(data: Mapping[str, Any]) -> AnyNodeSpec:
    kind = data.get("kind", "node")
    if kind == "subgraph":
        graph = data.get("graph")
        if not isinstance(graph, Mapping):
            raise SerializationError(
                f"subgraph {data.get('id')!r} has no 'graph' object"
            )
        return SubgraphSpec(
            node_id=_require(data, "id", str),
            graph=_graph_from_dict(graph),
            params=_params_from_dict(data.get("params", {})),
        )
    if kind != "node":
        raise SerializationError(
            f"unknown node kind {kind!r}; expected 'node' or 'subgraph'"
        )
    readiness = data.get("readiness", {"kind": "all"})
    if not isinstance(readiness, Mapping):
        raise SerializationError(
            f"node {data.get('id')!r} readiness must be an object, got "
            f"{type(readiness).__name__}"
        )
    return NodeSpec(
        node_id=_require(data, "id", str),
        type_name=_require(data, "type", str),
        params=_params_from_dict(data.get("params", {})),
        readiness=readiness_from_dict(readiness),
        on_error=_policy(data.get("on_error", ErrorPolicy.STOP), ErrorPolicy),
        priority=int(data.get("priority", 0)),
    )


def _edge_from_dict(data: Mapping[str, Any]) -> EdgeSpec:
    return EdgeSpec(
        src=_ref_from_dict(data, "src"),
        dst=_ref_from_dict(data, "dst"),
        capacity=data.get("capacity"),
        on_overflow=_policy(data.get("on_overflow", Overflow.ERROR), Overflow),
        transport=data.get("transport", EdgeTransport.MEMORY),
    )


def _input_from_dict(data: Mapping[str, Any]) -> GraphInput:
    targets = data.get("targets", ())
    if not isinstance(targets, list):
        raise SerializationError(
            f"graph input {data.get('name')!r} targets must be a list"
        )
    return GraphInput(
        name=_require(data, "name", str),
        targets=tuple(_ref_value(item, "target") for item in targets),
        type_tag=data.get("type_tag"),
    )


def _output_from_dict(data: Mapping[str, Any]) -> GraphOutput:
    return GraphOutput(
        name=_require(data, "name", str),
        source=_ref_from_dict(data, "source"),
        type_tag=data.get("type_tag"),
    )


def _ref_from_dict(data: Mapping[str, Any], key: str) -> PortRef:
    return _ref_value(data.get(key), key)


def _ref_value(ref: Any, where: str) -> PortRef:
    if not isinstance(ref, Mapping) or "node" not in ref or "port" not in ref:
        raise SerializationError(
            f"expected {where} to be an object with 'node' and 'port', got {ref!r}"
        )
    return PortRef(node=str(ref["node"]), port=str(ref["port"]))


def _params_from_dict(data: Any) -> dict[str, Any]:
    if not isinstance(data, Mapping):
        raise SerializationError(f"expected a params object, got {type(data).__name__}")
    return {str(name): _param_from_json(value) for name, value in data.items()}


def _param_from_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        if set(value) == {PARAM_REF_KEY}:
            return ParamRef(str(value[PARAM_REF_KEY]))
        return {str(key): _param_from_json(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_param_from_json(item) for item in value]
    return value


def _policy[P: StrEnum](value: Any, policy: type[P]) -> Any:
    """Coerce a serialized policy name to its member of ``policy``.

    A name that is not a member comes back unchanged rather than raising here, so
    :func:`dfg.validate.validate` stays the one place a bad policy is reported --
    with the list of the ones that exist, and as a ``Problem`` alongside everything
    else wrong with the graph rather than as the first exception out of the reader.
    """
    try:
        return policy(value)
    except ValueError:
        return value


def _require[T](data: Mapping[str, Any], key: str, kind: type[T]) -> T:
    value = data.get(key)
    if not isinstance(value, kind):
        raise SerializationError(
            f"expected {key!r} to be {kind.__name__}, got {value!r}"
        )
    return value
