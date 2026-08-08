"""Readiness rules: when a node may fire, and what it gets when it does.

Scheduling is two axes, not one. This is the per-node axis; :mod:`dfg.ordering` is
the graph-wide one. ``all`` + ``topological`` and ``all`` + ``level`` are both
sensible and behave differently, which is why the two are separate.

A rule answers *both* questions -- :meth:`ReadinessRule.is_ready` and
:meth:`ReadinessRule.take`. They are the same question ("what does a firing
consume?"), and splitting them across two policies would add a per-node axis the
contract does not have. It is also what lets a batching rule exist:
:class:`CountAtLeast` is how "fire when 512 samples are buffered" is expressed,
and so how a batch stays a very large sample rather than a second engine.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Callable, Mapping
from typing import Any, ClassVar, Protocol, runtime_checkable

from dfg.errors import SerializationError, UnknownReadinessError
from dfg.message import Message


@runtime_checkable
class PortQueue(Protocol):
    """What a rule sees of one input port's pending messages.

    The runtime supplies one of these per *declared* input port, so a rule can
    always index every port by name; a port with no writer is simply always empty.
    """

    def __len__(self) -> int:
        """Number of messages currently pending."""
        ...

    def peek(self, index: int = 0) -> Message[Any]:
        """Return a pending message without consuming it. ``0`` is the oldest."""
        ...

    def take(self, count: int) -> tuple[Message[Any], ...]:
        """Consume and return up to ``count`` messages, oldest first."""
        ...


class ReadinessRule(ABC):
    """Decides when a node may fire, and what that firing consumes.

    ``KIND`` is the name this rule serializes under. A rule whose kind is not
    registered may still be used on an in-memory blueprint -- it just cannot be
    written to JSON, which is checked when you try.
    """

    KIND: ClassVar[str] = ""

    @abstractmethod
    def is_ready(self, queues: Mapping[str, PortQueue]) -> bool:
        """Return whether the node may fire now.

        Must not consume anything: the scheduler asks every node before it picks
        one, so a rule that consumed here would eat messages for a node that does
        not then run.
        """

    def take(self, queues: Mapping[str, PortQueue]) -> dict[str, tuple[Message, ...]]:
        """Consume this firing's inputs. Defaults to one per non-empty port.

        Returns:
            Port name to the messages consumed, omitting ports that gave none.
            The mapping this returns is what the node's ``run`` receives.
        """
        taken: dict[str, tuple[Message, ...]] = {}
        for name, queue in queues.items():
            if len(queue) > 0:
                messages = queue.take(1)
                if messages:
                    taken[name] = messages
        return taken

    def to_dict(self) -> dict[str, Any]:
        """Return the serialized form. Subclasses add their own fields."""
        if not self.KIND:
            raise SerializationError(
                f"{type(self).__name__} has no KIND, so it cannot be serialized"
            )
        return {"kind": self.KIND}

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> ReadinessRule:
        """Build a rule of *this* class from ``data``. Ignores ``"kind"``."""
        del data
        return cls()  # type: ignore[abstract]

    def __eq__(self, other: object) -> bool:
        if type(other) is not type(self):
            return NotImplemented
        return self._identity() == other._identity()  # type: ignore[attr-defined]

    def __hash__(self) -> int:
        return hash((type(self).__name__, self._identity()))

    def _identity(self) -> tuple[Any, ...]:
        """The fields that make two rules of the same class equal."""
        return ()

    def __repr__(self) -> str:
        fields = ", ".join(repr(part) for part in self._identity())
        return f"{type(self).__name__}({fields})"


class AllInputs(ReadinessRule):
    """Fires when every input port has a message available."""

    KIND = "all"

    def is_ready(self, queues: Mapping[str, PortQueue]) -> bool:
        # A node with no input ports would vacuously satisfy `all` and then fire
        # forever. Validation rejects those (graph inputs are the only sources),
        # but the rule refuses on its own too rather than relying on that.
        return bool(queues) and all(len(queue) > 0 for queue in queues.values())


class AnyInput(ReadinessRule):
    """Fires when at least one input port has a message available."""

    KIND = "any"

    def is_ready(self, queues: Mapping[str, PortQueue]) -> bool:
        return any(len(queue) > 0 for queue in queues.values())


class CountAtLeast(ReadinessRule):
    """Fires when a port has buffered ``count`` messages, and consumes them all.

    This is the batching rule: framing audio into windows, accumulating rows into
    a columnar batch, or running the same graph over a whole recording at once.

    Attributes:
        count: How many messages the counted port(s) must have.
        port: The port to count, or ``None`` to require ``count`` on *every* port.
            Ports that are not counted still contribute one message each.
    """

    KIND = "count"

    def __init__(self, count: int, port: str | None = None) -> None:
        if count < 1:
            raise ValueError(f"count must be at least 1, got {count!r}")
        self.count = count
        self.port = port

    def _counted(self, queues: Mapping[str, PortQueue]) -> tuple[str, ...]:
        if self.port is None:
            return tuple(queues)
        return (self.port,) if self.port in queues else ()

    def is_ready(self, queues: Mapping[str, PortQueue]) -> bool:
        counted = self._counted(queues)
        if not counted:
            return False
        return all(len(queues[name]) >= self.count for name in counted)

    def take(self, queues: Mapping[str, PortQueue]) -> dict[str, tuple[Message, ...]]:
        counted = set(self._counted(queues))
        taken: dict[str, tuple[Message, ...]] = {}
        for name, queue in queues.items():
            wanted = self.count if name in counted else 1
            if len(queue) > 0:
                messages = queue.take(wanted)
                if messages:
                    taken[name] = messages
        return taken

    def to_dict(self) -> dict[str, Any]:
        return {"kind": self.KIND, "count": self.count, "port": self.port}

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> CountAtLeast:
        return cls(count=int(data["count"]), port=data.get("port"))

    def _identity(self) -> tuple[Any, ...]:
        return (self.count, self.port)


class PredicateRule(ReadinessRule):
    """An arbitrary predicate over the node's input queues.

    The contract calls custom rules extensible *and* calls blueprints
    serializable, and a callable is not both. The resolution: an in-memory
    blueprint may hold one of these and it runs fine, but writing that blueprint
    to JSON raises, naming the node. A rule you intend to persist should be a
    small class with a registered ``KIND`` instead -- see
    :func:`register_readiness`.
    """

    KIND = "predicate"

    def __init__(
        self,
        predicate: Callable[[Mapping[str, PortQueue]], bool],
        *,
        name: str = "<predicate>",
        take: Callable[[Mapping[str, PortQueue]], dict[str, tuple[Message, ...]]]
        | None = None,
    ) -> None:
        self.predicate = predicate
        self.name = name
        self._take = take

    def is_ready(self, queues: Mapping[str, PortQueue]) -> bool:
        return bool(self.predicate(queues))

    def take(self, queues: Mapping[str, PortQueue]) -> dict[str, tuple[Message, ...]]:
        if self._take is not None:
            return self._take(queues)
        return super().take(queues)

    def to_dict(self) -> dict[str, Any]:
        raise SerializationError(
            f"readiness rule {self.name!r} is a PredicateRule, which holds a "
            f"callable and cannot be serialized; give the rule a class with a "
            f"registered KIND if the blueprint needs to round-trip"
        )

    def _identity(self) -> tuple[Any, ...]:
        return (self.name, self.predicate, self._take)

    def __repr__(self) -> str:
        return f"PredicateRule({self.name!r})"


_KINDS: dict[str, type[ReadinessRule]] = {}


def register_readiness(rule_cls: type[ReadinessRule]) -> type[ReadinessRule]:
    """Register a rule class so blueprints using it can round-trip.

    Usable as a decorator.

    Raises:
        ValueError: If the class has no ``KIND``, or the kind is already taken.
    """
    kind = rule_cls.KIND
    if not kind:
        raise ValueError(f"{rule_cls.__name__} must set KIND to be registered")
    existing = _KINDS.get(kind)
    if existing is not None and existing is not rule_cls:
        raise ValueError(
            f"readiness kind {kind!r} is already registered to {existing.__name__}"
        )
    _KINDS[kind] = rule_cls
    return rule_cls


def readiness_kinds() -> tuple[str, ...]:
    """Registered readiness kinds, sorted."""
    return tuple(sorted(_KINDS))


def is_registered_kind(kind: str) -> bool:
    """Return whether ``kind`` names a registered readiness rule."""
    return kind in _KINDS


def readiness_from_dict(data: Mapping[str, Any]) -> ReadinessRule:
    """Rebuild a rule from its serialized form.

    Raises:
        UnknownReadinessError: If the kind is not registered.
    """
    kind = data.get("kind")
    rule_cls = _KINDS.get(kind) if isinstance(kind, str) else None
    if rule_cls is None:
        raise UnknownReadinessError(
            f"readiness kind {kind!r} is not registered; "
            f"registered kinds are {list(readiness_kinds())}"
        )
    return rule_cls.from_dict(data)


register_readiness(AllInputs)
register_readiness(AnyInput)
register_readiness(CountAtLeast)
# PredicateRule is deliberately NOT registered: it cannot round-trip, and
# registering it would make a blueprint look persistable when it is not.
