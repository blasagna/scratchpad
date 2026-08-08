"""Ports, and the naming rules that make qualified IDs and topics parseable.

A port is a named attachment point on a node. Its optional type tag is a
free-form **string**, not a Python type: a type object does not survive JSON, and
a tag has to mean the same thing to a future port in another language. Untyped is
the default, so validation compares tags only where an author supplied both ends.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
"""Node IDs, port names, and graph boundary names all match this.

Notably it excludes ``.``, which is what lets a qualified ID and a topic be one
flat dotted path that a reader can split.
"""

RESERVED_PREFIX = "__"
"""Framework-owned port names start here (``__error__``); authors' may not."""

ERROR_PORT = "__error__"
"""The port name the ``route`` error policy publishes on. Tap-only, never wired."""


@dataclass(frozen=True, slots=True)
class PortSpec:
    """A named, optionally typed attachment point on a node.

    Attributes:
        name: Must satisfy :data:`NAME_RE`.
        type_tag: An opaque string an author chooses, or ``None`` for untyped.
            Validation rejects a connection only when both ends carry a tag and
            the tags differ.
        description: Free text, carried into rendered diagrams and nothing else.
    """

    name: str
    type_tag: str | None = None
    description: str = ""


def is_valid_name(name: str) -> bool:
    """Return whether ``name`` is a legal node ID, port name, or boundary name."""
    return bool(NAME_RE.match(name))


def is_reserved_name(name: str) -> bool:
    """Return whether ``name`` is in the framework's reserved namespace."""
    return name.startswith(RESERVED_PREFIX)


def qualify(scope: tuple[str, ...], node_id: str) -> str:
    """Join enclosing subgraph IDs and a node ID into a qualified node ID.

    The root graph contributes no prefix, so a top-level node is just its own ID.

    >>> qualify((), "calib")
    'calib'
    >>> qualify(("fusion",), "predict")
    'fusion.predict'
    """
    return ".".join((*scope, node_id))


def topic_of(qualified_id: str, port_name: str) -> str:
    """Return the topic naming an output port.

    The same ``.`` separates subgraph boundaries and the port boundary, so a
    topic is one flat path from the root graph down to a port. A topic and a
    qualified node ID are therefore spelled alike and told apart by context: a
    topic always ends in a port name.

    >>> topic_of("fusion.update", "fused")
    'fusion.update.fused'
    """
    return f"{qualified_id}.{port_name}"


def by_name(ports: tuple[PortSpec, ...]) -> dict[str, PortSpec]:
    """Index a port tuple by name, preserving declaration order."""
    return {port.name: port for port in ports}
