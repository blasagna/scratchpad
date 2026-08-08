"""The mapping from a node type name to a factory.

Deserializing a blueprint without a registry produces a description nothing can
instantiate, which is why the registry is a concept in the contract rather than an
implementation detail. It is also what makes validation possible before any node
is constructed: the registry answers "what ports and parameters does type ``X``
have" from the class, without calling ``__init__``.

**There is no global default registry.** An explicit instance keeps tests
isolated, and it makes the layering above a fact about the API instead of a
remark. Type names are serialized, so they are API: keep them short and stable
(``"imu.calibrate"``, ``"audio.frame"``).
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
from typing import Any

from dfg.errors import UnknownNodeTypeError
from dfg.node import Node
from dfg.ports import PortSpec

type NodeFactory = Callable[..., Node]


@dataclass(frozen=True, slots=True)
class NodeTypeInfo:
    """What the registry can say about a type without instantiating it.

    Attributes:
        type_name: The registered name, as it appears in a serialized blueprint.
        factory: What :meth:`Registry.create` calls.
        params: Parameter name to default, mirroring ``Node.PARAMS``.
        mutable_params: Parameter names the control plane may change live.
        node_cls: The class, when one was registered; ``None`` for a bare factory.
    """

    type_name: str
    factory: NodeFactory
    params: Mapping[str, Any]
    mutable_params: frozenset[str]
    node_cls: type[Node] | None
    _inputs: tuple[PortSpec, ...]
    _outputs: tuple[PortSpec, ...]

    def input_ports(self, params: Mapping[str, Any]) -> tuple[PortSpec, ...]:
        """Input ports for a node built with ``params``, without building one."""
        if self.node_cls is not None:
            return self.node_cls.input_ports(params)
        return self._inputs

    def output_ports(self, params: Mapping[str, Any]) -> tuple[PortSpec, ...]:
        """Output ports for a node built with ``params``, without building one."""
        if self.node_cls is not None:
            return self.node_cls.output_ports(params)
        return self._outputs


class Registry:
    """A mapping from node type names to factories.

    >>> registry = Registry()
    >>> @registry.node("demo.double")
    ... class Double(Node):
    ...     INPUTS = (PortSpec("in"),)
    ...     OUTPUTS = (PortSpec("out"),)
    ...     def run(self, inputs):
    ...         return None
    >>> "demo.double" in registry
    True
    """

    def __init__(self) -> None:
        self._types: dict[str, NodeTypeInfo] = {}

    def node(self, type_name: str) -> Callable[[type[Node]], type[Node]]:
        """Return a decorator registering a :class:`~dfg.node.Node` subclass."""

        def decorate(node_cls: type[Node]) -> type[Node]:
            self.register(type_name, node_cls)
            return node_cls

        return decorate

    def register(self, type_name: str, node_cls: type[Node]) -> None:
        """Register ``node_cls`` under ``type_name``.

        Raises:
            ValueError: If the name is already registered, or ``node_cls`` is not a
                :class:`~dfg.node.Node` subclass.
        """
        if type_name in self._types:
            raise ValueError(f"node type {type_name!r} is already registered")
        if not (isinstance(node_cls, type) and issubclass(node_cls, Node)):
            raise ValueError(
                f"{type_name!r}: expected a Node subclass, got {node_cls!r}"
            )
        self._types[type_name] = NodeTypeInfo(
            type_name=type_name,
            factory=node_cls,
            params=dict(node_cls.PARAMS),
            mutable_params=frozenset(node_cls.MUTABLE_PARAMS),
            node_cls=node_cls,
            _inputs=node_cls.INPUTS,
            _outputs=node_cls.OUTPUTS,
        )

    def register_factory(
        self,
        type_name: str,
        factory: NodeFactory,
        *,
        inputs: tuple[PortSpec, ...] = (),
        outputs: tuple[PortSpec, ...] = (),
        params: Mapping[str, Any] | None = None,
        mutable_params: frozenset[str] = frozenset(),
    ) -> None:
        """Register a callable that is not a class.

        The port and parameter descriptors have to be passed explicitly, because
        the "validate before you construct" contract has to hold for a closure
        just as it does for a class.

        Raises:
            ValueError: If the name is already registered.
        """
        if type_name in self._types:
            raise ValueError(f"node type {type_name!r} is already registered")
        self._types[type_name] = NodeTypeInfo(
            type_name=type_name,
            factory=factory,
            params=dict(params or {}),
            mutable_params=frozenset(mutable_params),
            node_cls=None,
            _inputs=inputs,
            _outputs=outputs,
        )

    def create(self, type_name: str, params: Mapping[str, Any]) -> Node:
        """Build a node instance.

        Raises:
            UnknownNodeTypeError: If ``type_name`` is not registered.
        """
        return self.describe_or_raise(type_name).factory(**params)

    def describe(self, type_name: str) -> NodeTypeInfo | None:
        """Return what is known about ``type_name``, or ``None`` if unregistered."""
        return self._types.get(type_name)

    def describe_or_raise(self, type_name: str) -> NodeTypeInfo:
        """Return what is known about ``type_name``.

        Raises:
            UnknownNodeTypeError: If ``type_name`` is not registered. This is the
                error a blueprint loaded without a registry produces at
                instantiation time, and it names the registered alternatives.
        """
        info = self._types.get(type_name)
        if info is None:
            raise UnknownNodeTypeError(
                f"node type {type_name!r} is not registered; "
                f"registered types are {list(self.names())}"
            )
        return info

    def names(self) -> tuple[str, ...]:
        """Registered type names, sorted."""
        return tuple(sorted(self._types))

    def __contains__(self, type_name: object) -> bool:
        return type_name in self._types

    def __len__(self) -> int:
        return len(self._types)

    def __repr__(self) -> str:
        return f"Registry({list(self.names())!r})"

    @classmethod
    def merged(cls, *registries: Registry) -> Registry:
        """Combine registries into a new one.

        Raises:
            ValueError: On a type name registered in more than one of them. A
                silent last-wins would let an example quietly shadow a core node
                type and change what a serialized blueprint means.
        """
        merged = cls()
        for registry in registries:
            for type_name, info in registry._types.items():
                if type_name in merged._types:
                    raise ValueError(
                        f"node type {type_name!r} is registered in more than one "
                        f"registry being merged"
                    )
                merged._types[type_name] = info
        return merged
