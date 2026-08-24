"""The blueprint: a serializable description of a graph that cannot process data.

The blueprint layer and the instance layer never merge. Everything here is frozen,
cheap to build, and holds no runtime state; :class:`dfg.graph.Graph` is what turns
one into something runnable. Nothing in this module imports the runtime.

Two shape decisions are worth knowing:

**Boundary wiring lives on the boundary declaration, not in ``edges``.**
:attr:`GraphOutput.source` is a single :class:`PortRef`, so "a graph output name is
an *alias* of the connected node's output" is enforced by the type rather than by a
post-hoc "exactly one edge reaches this output" check. :attr:`GraphInput.targets`
is a tuple, so one injected stream fanning out to several inner ports is one
declaration. The alternative -- a reserved node name standing for the boundary
inside ``edges`` -- gives one mechanism instead of two, at the cost of making the
alias cardinality a runtime check and putting a magic string in every author's
edge list.

**Node IDs may not contain a dot.** That is what lets a qualified ID and a topic be
one flat dotted path, and lets ``"scale.scaled"`` split into node and port.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
from enum import StrEnum, auto
from typing import Any

from dfg.node import Node
from dfg.readiness import AllInputs, ReadinessRule


class ErrorPolicy(StrEnum):
    """What happens when a node's ``run`` raises.

    :attr:`STOP` is the default: a silent continue hides bugs in exactly the case
    where nobody is watching. Members are ``str``, and ``auto()`` spells each one as
    its lowercased name, so a member *is* its serialized form -- ``ErrorPolicy.DROP
    == "drop"``, and a blueprint written with the bare string still works.
    """

    STOP = auto()
    DROP = auto()
    ROUTE = auto()

    # Everything that carries one is annotated `ErrorPolicy | str`, because that is
    # the truth: a spec holds what the author or the JSON said, and `validate` is
    # what decides whether it is a legal spelling. Narrowing the annotation to the
    # enum would make the bare string this docstring promises a type error.


class Overflow(StrEnum):
    """What a bounded edge does when it is full.

    ``block`` is deliberately absent. It is the obvious fourth policy and it
    deadlocks a single-threaded scheduler instantly: the producer waits for space
    only the consumer can free, and the consumer only runs when the producer
    returns.
    """

    ERROR = auto()
    DROP_OLDEST = auto()
    DROP_NEWEST = auto()


class EdgeTransport(StrEnum):
    """The mechanism an edge uses to move messages.

    Unlike :class:`ErrorPolicy` and :class:`Overflow`, this enum is *open*: it names
    the transports the framework ships, not every legal value. Others are a
    registration away, not an API change, so :attr:`EdgeSpec.transport` stays typed
    ``str`` and an edge naming a transport registered elsewhere is well-formed.
    Members are ``str``, so ``EdgeTransport.MEMORY == "memory"`` and the registry
    lookup does not care which spelling an author used.
    """

    MEMORY = auto()


@dataclass(frozen=True, slots=True)
class PortRef:
    """One end of an edge: a sibling node's ID and one of its port names.

    ``node`` is always a sibling within the graph that declares the edge, never a
    dotted path -- qualified IDs are computed at flatten time, not written by hand.
    It may name a :class:`SubgraphSpec`, in which case ``port`` must be one of that
    subgraph's declared boundary names.
    """

    node: str
    port: str

    def __str__(self) -> str:
        return f"{self.node}.{self.port}"


@dataclass(frozen=True, slots=True)
class ParamRef:
    """A node parameter that takes its value from the enclosing graph's parameters.

    Serializes as ``{"$param": "name"}`` and is resolved at flatten time against
    the subgraph's own parameters merged with the overrides on the subgraph node.
    This is what makes a subgraph reusable -- the same ``classify`` blueprint at
    one threshold in one parent and another threshold elsewhere.
    """

    name: str

    def __str__(self) -> str:
        return f"${self.name}"


@dataclass(frozen=True, slots=True)
class NodeSpec:
    """A leaf node in a blueprint.

    Attributes:
        node_id: Unique among its siblings. No dots.
        type_name: A registered node type name. Resolved by a
            :class:`dfg.registry.Registry`.
        params: Values passed to the factory. A value may be a :class:`ParamRef`.
        readiness: When this node may fire. Defaults to :class:`~dfg.readiness.AllInputs`.
        on_error: What happens when ``run`` raises.
        priority: Used only by the ``priority`` ordering; higher fires first.
    """

    node_id: str
    type_name: str
    params: Mapping[str, Any] = field(default_factory=dict)
    readiness: ReadinessRule = AllInputs()
    on_error: ErrorPolicy | str = ErrorPolicy.STOP
    priority: int = 0

    def __post_init__(self) -> None:
        object.__setattr__(self, "params", dict(self.params))


@dataclass(frozen=True, slots=True)
class SubgraphSpec:
    """A graph referenced as a node inside another graph.

    A graph has inputs, outputs, and parameters, which is exactly what lets it be
    used here. It has no readiness rule and no error policy of its own: flattening
    dissolves the boundary and those policies live on leaves. The cost is that you
    cannot say "this whole subgraph drops errors" in one place.

    Attributes:
        node_id: Unique among its siblings, and the namespace prefix for everything
            inside it. No dots.
        graph: The blueprint being referenced, inline.
        params: Overrides for ``graph.params``, which :class:`ParamRef`\\ s inside
            resolve against.
    """

    node_id: str
    graph: GraphSpec
    params: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "params", dict(self.params))


type AnyNodeSpec = NodeSpec | SubgraphSpec


@dataclass(frozen=True, slots=True)
class EdgeSpec:
    """A connection from one output port to one input port.

    Attributes:
        src: The producing output port.
        dst: The consuming input port. Only one writer per input port is allowed --
            two producers into one queue would make message order depend on firing
            interleaving, which determinism cannot tolerate. Use a merge node with
            N input ports instead.
        capacity: ``None`` for unbounded, which is the default for in-memory edges
            under the single-threaded scheduler: a run proceeds to quiescence, so a
            bound there only converts a working graph into a failing one.
        on_overflow: What to do when a bounded edge is full.
        transport: The mechanism this edge uses. A registered name, which may be an
            :class:`EdgeTransport` member or any other name a runtime has registered.
    """

    src: PortRef
    dst: PortRef
    capacity: int | None = None
    on_overflow: Overflow | str = Overflow.ERROR
    transport: str = EdgeTransport.MEMORY

    def __post_init__(self) -> None:
        if self.capacity is not None and self.capacity < 1:
            raise ValueError(
                f"edge {self.src} -> {self.dst}: capacity must be at least 1 or "
                f"None for unbounded, got {self.capacity!r}"
            )

    def __str__(self) -> str:
        return f"{self.src} -> {self.dst}"


@dataclass(frozen=True, slots=True)
class GraphInput:
    """A named entry point the application injects into.

    Graph inputs are the only sources: every node fires because something upstream
    produced, which is most of what makes a recorded input sequence fully determine
    a run.

    Attributes:
        name: Unique among the declaring graph's input names.
        targets: One or more inner input ports this feeds. Several targets is a
            fan-out, and still one injected message per target-set.
        type_tag: Optional, checked against the target ports where both are set.
    """

    name: str
    targets: tuple[PortRef, ...]
    type_tag: str | None = None


@dataclass(frozen=True, slots=True)
class GraphOutput:
    """A named exit point, which is an *alias* of one node output port.

    Subscribing to the alias and subscribing to the aliased topic observe the same
    port and the same messages. An alias resolves within the graph that declares
    it, so names only have to be unique among their siblings.

    Attributes:
        name: Unique among the declaring graph's output names.
        source: The single output port this name aliases.
        type_tag: Optional, checked against ``source`` where both are set.
    """

    name: str
    source: PortRef
    type_tag: str | None = None


@dataclass(frozen=True, slots=True)
class GraphSpec:
    """A set of nodes and edges, plus its own inputs, outputs, and parameters.

    Because it has all three, it can be referenced as a node inside a higher-level
    graph -- see :class:`SubgraphSpec`.
    """

    name: str
    nodes: tuple[AnyNodeSpec, ...] = ()
    edges: tuple[EdgeSpec, ...] = ()
    inputs: tuple[GraphInput, ...] = ()
    outputs: tuple[GraphOutput, ...] = ()
    params: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "params", dict(self.params))

    def node(self, node_id: str) -> AnyNodeSpec | None:
        """Return the child spec with ``node_id``, or ``None``."""
        for spec in self.nodes:
            if spec.node_id == node_id:
                return spec
        return None

    def input(self, name: str) -> GraphInput | None:
        """Return the declared input named ``name``, or ``None``."""
        for boundary in self.inputs:
            if boundary.name == name:
                return boundary
        return None

    def output(self, name: str) -> GraphOutput | None:
        """Return the declared output named ``name``, or ``None``."""
        for boundary in self.outputs:
            if boundary.name == name:
                return boundary
        return None


def split_endpoint(endpoint: str) -> PortRef:
    """Parse a ``"node.port"`` endpoint into a :class:`PortRef`.

    Split at the *last* dot. Sibling node IDs contain no dots, so there is only
    one, but splitting at the last is the rule that keeps working if that ever
    changes.

    Raises:
        ValueError: If ``endpoint`` has no dot.

    >>> split_endpoint("scale.scaled")
    PortRef(node='scale', port='scaled')
    """
    node, dot, port = endpoint.rpartition(".")
    if not dot:
        raise ValueError(
            f"endpoint {endpoint!r} must be spelled 'node.port'; it has no dot"
        )
    return PortRef(node=node, port=port)


class GraphBuilder:
    """A mutable front end that produces an immutable :class:`GraphSpec`.

    Literal nested tuples of frozen specs are unreadable, and a blueprint is
    something authors write by hand.

    >>> builder = GraphBuilder("chain")
    >>> _ = builder.add("double", "demo.double")
    >>> builder.add_input("samples", "double.inp")
    >>> builder.add_output("result", "double.output")
    >>> spec = builder.build()
    >>> spec.name, len(spec.nodes)
    ('chain', 1)

    A registered node type may also be passed by class instead of by name, so a
    typo or the wrong class is caught at the call site rather than at validation:

    >>> from typing import NamedTuple
    >>> from dfg.node import Emit, In
    >>> class Double(Node):
    ...     class Out(NamedTuple):
    ...         output: Emit[int]
    ...     def run(self, *, inp: In[int] = ()) -> Out:
    ...         return self.Out(output=inp)
    >>> by_class = GraphBuilder("chain2")
    >>> _ = by_class.add("double", Double)
    >>> by_class.build().nodes[0].type_name  # doctest: +ELLIPSIS
    '...Double'
    """

    def __init__(self, name: str, *, params: Mapping[str, Any] | None = None) -> None:
        self.name = name
        self.params: dict[str, Any] = dict(params or {})
        self._nodes: list[AnyNodeSpec] = []
        self._edges: list[EdgeSpec] = []
        self._inputs: list[GraphInput] = []
        self._outputs: list[GraphOutput] = []

    def add(
        self,
        node_id: str,
        type_name: type[Node] | str,
        *,
        params: Mapping[str, Any] | None = None,
        readiness: ReadinessRule | None = None,
        on_error: ErrorPolicy | str = ErrorPolicy.STOP,
        priority: int = 0,
    ) -> str:
        """Add a leaf node. Returns ``node_id``, so it reads well inline.

        ``type_name`` may be a registered node type name, or the
        :class:`~dfg.node.Node` subclass itself -- ``add(id, SomeNode)`` derives
        the name from the class's own import path, the same name it must be
        registered under (e.g. via ``registry.register(SomeNode)``).

        Raises:
            TypeError: If ``type_name`` is a class that is not a Node subclass.
        """
        if isinstance(type_name, type):
            if not issubclass(type_name, Node):
                raise TypeError(
                    f"add() with a class expects a Node subclass, got {type_name!r}"
                )
            type_name = f"{type_name.__module__}.{type_name.__qualname__}"
        self._nodes.append(
            NodeSpec(
                node_id=node_id,
                type_name=type_name,
                params=dict(params or {}),
                readiness=readiness if readiness is not None else AllInputs(),
                on_error=on_error,
                priority=priority,
            )
        )
        return node_id

    def add_subgraph(
        self,
        node_id: str,
        graph: GraphSpec,
        *,
        params: Mapping[str, Any] | None = None,
    ) -> str:
        """Reference ``graph`` as a node. Returns ``node_id``."""
        self._nodes.append(
            SubgraphSpec(node_id=node_id, graph=graph, params=dict(params or {}))
        )
        return node_id

    def connect(
        self,
        src: str,
        dst: str,
        *,
        capacity: int | None = None,
        on_overflow: Overflow | str = Overflow.ERROR,
        transport: str = EdgeTransport.MEMORY,
    ) -> None:
        """Connect ``"node.port"`` to ``"node.port"``."""
        self._edges.append(
            EdgeSpec(
                src=split_endpoint(src),
                dst=split_endpoint(dst),
                capacity=capacity,
                on_overflow=on_overflow,
                transport=transport,
            )
        )

    def add_input(self, name: str, *targets: str, type_tag: str | None = None) -> None:
        """Declare a graph input feeding one or more ``"node.port"`` targets."""
        self._inputs.append(
            GraphInput(
                name=name,
                targets=tuple(split_endpoint(t) for t in targets),
                type_tag=type_tag,
            )
        )

    def add_output(self, name: str, source: str, type_tag: str | None = None) -> None:
        """Declare a graph output aliasing the ``"node.port"`` given by ``source``."""
        self._outputs.append(
            GraphOutput(name=name, source=split_endpoint(source), type_tag=type_tag)
        )

    def build(self) -> GraphSpec:
        """Freeze what has been added into a :class:`GraphSpec`.

        Does not validate -- :func:`dfg.validate.validate` does, and it needs a
        registry to resolve type names.
        """
        return GraphSpec(
            name=self.name,
            nodes=tuple(self._nodes),
            edges=tuple(self._edges),
            inputs=tuple(self._inputs),
            outputs=tuple(self._outputs),
            params=dict(self.params),
        )
