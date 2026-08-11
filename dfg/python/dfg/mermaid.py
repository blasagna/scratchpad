"""Rendering a blueprint as a mermaid flowchart.

This renders the **blueprint**, not a running graph: subgraphs stay nested, and no
node has been constructed. That is the point -- a blueprint is renderable before it
is runnable, so a diagram is something you can get from a description that does not
even validate yet.

Output is deterministic: declaration order throughout, no dict iteration, no set
ordering. A golden-string test can therefore compare it byte for byte.

Graphviz is deliberately not here. The contract's demo list names mermaid, and one
renderer that matches the document's own diagrams is worth more than two that
half-match.
"""

from __future__ import annotations

from dfg.blueprint import ErrorPolicy, GraphSpec, NodeSpec, SubgraphSpec
from dfg.ports import qualify, topic_of

DIRECTIONS = frozenset(("LR", "RL", "TB", "BT", "TD"))


def render_mermaid(
    spec: GraphSpec, *, direction: str = "LR", show_topics: bool = True
) -> str:
    """Render ``spec`` as a mermaid ``flowchart``.

    Args:
        spec: The blueprint to draw, nested subgraphs and all.
        direction: A mermaid flowchart direction, e.g. ``"LR"`` or ``"TB"``.
        show_topics: Label each edge with its producing port's canonical name, so
            the labels double as a check that the namespacing came out how the
            author expected. Where the producer is a subgraph's boundary port the
            label is that graph output's *alias* (``fusion.pose``) rather than the
            topic it resolves to (``fusion.update.fused``) -- this draws the
            blueprint as declared, and nothing has been flattened yet.

    Returns:
        The diagram, without a fence. Paste it inside a ```` ```mermaid ```` block.

    Raises:
        ValueError: If ``direction`` is not a mermaid direction.
    """
    if direction not in DIRECTIONS:
        raise ValueError(f"direction {direction!r} is not one of {sorted(DIRECTIONS)}")
    lines = [f"flowchart {direction}"]
    lines.extend(_boundary_lines(spec, scope=(), show_topics=show_topics))
    lines.extend(_body_lines(spec, scope=(), show_topics=show_topics))
    return "\n".join(lines)


def _boundary_lines(
    spec: GraphSpec, *, scope: tuple[str, ...], show_topics: bool
) -> list[str]:
    """Graph inputs and outputs, drawn as stadium nodes feeding the real ones."""
    lines: list[str] = []
    for boundary in spec.inputs:
        node_id = _boundary_id(scope, "in", boundary.name)
        lines.append(f"  {node_id}([{boundary.name}])")
        for target in boundary.targets:
            lines.append(f"  {node_id} --> {_ref_id(scope, target.node)}")
    for boundary in spec.outputs:
        node_id = _boundary_id(scope, "out", boundary.name)
        lines.append(f"  {node_id}([{boundary.name}])")
        source = boundary.source
        src_id = _ref_id(scope, source.node)
        if show_topics:
            label = topic_of(qualify(scope, source.node), source.port)
            lines.append(f'  {src_id} -- "{label}" --> {node_id}')
        else:
            lines.append(f"  {src_id} --> {node_id}")
    return lines


def _body_lines(
    spec: GraphSpec, *, scope: tuple[str, ...], show_topics: bool
) -> list[str]:
    """Nodes, nested subgraph blocks, and the edges declared at this level."""
    lines: list[str] = []
    for child in spec.nodes:
        if isinstance(child, SubgraphSpec):
            inner_scope = (*scope, child.node_id)
            lines.append(f"  subgraph {_ref_id(scope, child.node_id)}[{child.node_id}]")
            lines.extend(
                "  " + line
                for line in _body_lines(
                    child.graph, scope=inner_scope, show_topics=show_topics
                )
            )
            lines.append("  end")
        else:
            lines.append(f"  {_ref_id(scope, child.node_id)}[{_node_label(child)}]")

    for edge in spec.edges:
        src = _ref_id(scope, edge.src.node)
        dst = _ref_id(scope, edge.dst.node)
        if show_topics:
            label = topic_of(qualify(scope, edge.src.node), edge.src.port)
            lines.append(f'  {src} -- "{label}" --> {dst}')
        else:
            lines.append(f"  {src} --> {dst}")
    return lines


def _node_label(child: NodeSpec) -> str:
    """A node's box label: its ID, plus a marker for a non-default policy."""
    marks = []
    if child.readiness.KIND and child.readiness.KIND != "all":
        marks.append(child.readiness.KIND)
    if child.on_error != ErrorPolicy.STOP:
        marks.append(str(child.on_error))
    return f"{child.node_id}<br/>{'/'.join(marks)}" if marks else child.node_id


def _ref_id(scope: tuple[str, ...], node_id: str) -> str:
    """A mermaid-safe identifier for a node, unique across nesting levels."""
    return _sanitize(qualify(scope, node_id))


def _boundary_id(scope: tuple[str, ...], kind: str, name: str) -> str:
    """A mermaid-safe identifier for a boundary stadium node.

    Prefixed by direction because a graph may declare an input and an output with
    the same name, and they are different things.
    """
    return _sanitize(f"{qualify(scope, name)}__{kind}")


def _sanitize(text: str) -> str:
    """Mermaid identifiers may not contain a dot."""
    return text.replace(".", "__")
