"""Orderings: which of the ready nodes fires next.

This is the graph-wide scheduling axis; :mod:`dfg.readiness` is the per-node one.

**Ties must break deterministically, and they break in exactly one place.** A
topological order is not unique and neither is the set of ready nodes at a given
level, so every ordering exposes a *sort key* and :meth:`Ordering.pick` is
implemented once on the base class as ``min(ready, key=self.key)``. The rule is
that a key's last element is the qualified node ID, and a test asserts it for every
built-in. An ordering that forgets fails that test, rather than silently making two
replays of the same recording diverge.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Sequence
from typing import Any, ClassVar

from dfg.flatten import FlatGraph, levels, topological_order


class Ordering(ABC):
    """Picks which ready node runs next.

    Subclasses implement :meth:`key`. They may override :meth:`prepare` to
    precompute from the graph, which is called once at instantiation.
    """

    NAME: ClassVar[str] = ""

    def prepare(self, flat: FlatGraph) -> None:
        """Precompute whatever :meth:`key` needs. Called once, before any firing."""

    @abstractmethod
    def key(self, qid: str) -> tuple[Any, ...]:
        """Return a sort key for a ready node; lowest wins.

        The **last element must be** ``qid``. That is the deterministic tie-break
        the contract requires, and it is why declaration order cannot leak into
        the output of a run.
        """

    def pick(self, ready: Sequence[str]) -> str:
        """Return the ready node that fires next.

        The only place in the codebase where a tie is broken.
        """
        return min(ready, key=self.key)

    def __repr__(self) -> str:
        return f"{type(self).__name__}()"


class TopologicalOrdering(Ordering):
    """The ready node earliest in a topological sort of the graph.

    The sort itself is canonical -- Kahn's algorithm with a min-heap keyed by
    qualified ID -- so by the time a key is built there are no ties left to break.
    The ``qid`` on the end is belt and braces, and keeps the rule uniform.
    """

    NAME = "topological"

    def __init__(self) -> None:
        self._index: dict[str, int] = {}

    def prepare(self, flat: FlatGraph) -> None:
        self._index = {qid: i for i, qid in enumerate(topological_order(flat))}

    def key(self, qid: str) -> tuple[Any, ...]:
        return (self._index.get(qid, len(self._index)), qid)


class LevelOrdering(Ordering):
    """Ready nodes level by level, breadth-first from the graph inputs.

    Levels are longest-path (``1 + max(pred)``), so a node is never at a lower
    level than something feeding it. Shortest-path levels would let a node fire
    ahead of its own predecessor, and then this ordering and the topological one
    would not agree on outputs.
    """

    NAME = "level"

    def __init__(self) -> None:
        self._levels: dict[str, int] = {}

    def prepare(self, flat: FlatGraph) -> None:
        self._levels = levels(flat)

    def key(self, qid: str) -> tuple[Any, ...]:
        return (self._levels.get(qid, 0), qid)


class PriorityOrdering(Ordering):
    """The ready node with the highest author-assigned priority.

    Priority comes from the blueprint's ``priority`` field. Negated in the key
    because lowest wins, so a higher number fires first.
    """

    NAME = "priority"

    def __init__(self) -> None:
        self._priority: dict[str, int] = {}

    def prepare(self, flat: FlatGraph) -> None:
        self._priority = {qid: node.priority for qid, node in flat.nodes.items()}

    def key(self, qid: str) -> tuple[Any, ...]:
        return (-self._priority.get(qid, 0), qid)


_ORDERINGS: dict[str, type[Ordering]] = {
    TopologicalOrdering.NAME: TopologicalOrdering,
    LevelOrdering.NAME: LevelOrdering,
    PriorityOrdering.NAME: PriorityOrdering,
}


def register_ordering(ordering_cls: type[Ordering]) -> type[Ordering]:
    """Register an ordering class under its ``NAME``. Usable as a decorator.

    Raises:
        ValueError: If the class has no ``NAME``, or the name is already taken.
    """
    name = ordering_cls.NAME
    if not name:
        raise ValueError(f"{ordering_cls.__name__} must set NAME to be registered")
    existing = _ORDERINGS.get(name)
    if existing is not None and existing is not ordering_cls:
        raise ValueError(
            f"ordering {name!r} is already registered to {existing.__name__}"
        )
    _ORDERINGS[name] = ordering_cls
    return ordering_cls


def ordering_names() -> tuple[str, ...]:
    """Registered ordering names, sorted."""
    return tuple(sorted(_ORDERINGS))


def make_ordering(name: str) -> Ordering:
    """Build an ordering by name.

    Raises:
        ValueError: If ``name`` is not registered.
    """
    ordering_cls = _ORDERINGS.get(name)
    if ordering_cls is None:
        raise ValueError(
            f"unknown ordering {name!r}; registered orderings are "
            f"{list(ordering_names())}"
        )
    return ordering_cls()
