"""The exception hierarchy.

Every error the framework raises derives from :class:`DfgError`, so an embedding
application can catch the framework without catching its nodes' own failures --
except for :class:`NodeRunError`, which deliberately wraps a node's exception so
that the qualified ID of the node that failed is part of the message.

The split mirrors the layering in ``README.md``: :class:`ValidationError` and
:class:`SerializationError` belong to the blueprint layer and can be raised with
no node ever constructed, while :class:`NodeRunError`, :class:`EdgeOverflowError`,
and :class:`LifecycleError` can only happen once there is a running graph.
"""

from __future__ import annotations

from dataclasses import dataclass, field


class DfgError(Exception):
    """Base class for every error raised by the framework itself."""


# --- Blueprint layer ---------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Problem:
    """One thing wrong with a blueprint.

    Attributes:
        code: A stable identifier -- ``"duplicate_id"``, ``"dangling_edge"``,
            ``"cycle"``. Tests assert on these rather than on wording.
        detail: What is wrong, in a sentence.
        scope: The enclosing subgraph IDs of the graph that declares the problem,
            so a message can name the right place in a nested blueprint.
    """

    code: str
    detail: str
    scope: tuple[str, ...] = field(default_factory=tuple)

    def __str__(self) -> str:
        where = ".".join(self.scope) if self.scope else "<root>"
        return f"[{self.code}] in {where}: {self.detail}"


class ValidationError(DfgError):
    """A blueprint is not valid.

    Carries *every* problem found, not just the first: a half-built graph usually
    has several, and reporting them one run at a time is a poor way to fix them.
    """

    def __init__(self, problems: tuple[Problem, ...]) -> None:
        self.problems = problems
        lines = [f"blueprint has {len(problems)} problem(s):"]
        lines.extend(f"  - {p}" for p in problems)
        super().__init__("\n".join(lines))


class SerializationError(DfgError):
    """A blueprint cannot be written to (or read from) its JSON form."""


class SchemaVersionError(SerializationError):
    """A serialized blueprint declares a schema version this port cannot read."""


class UnknownNodeTypeError(DfgError):
    """A blueprint names a node type the registry cannot resolve.

    Deserializing without a registry is legal and produces a description nothing
    can instantiate, so this surfaces at instantiation rather than at load.
    """


class UnknownTransportError(DfgError):
    """An edge names a transport the registry cannot resolve."""


class UnknownReadinessError(DfgError):
    """A serialized readiness rule names a kind the registry cannot resolve."""


class ParamError(DfgError):
    """A node was given an unknown parameter, or is missing a required one."""


class ImmutableParamError(DfgError):
    """The control plane was asked to change a parameter that is not mutable.

    Parameters are immutable by default; a node opts a name into live changes by
    listing it in ``MUTABLE_PARAMS``.
    """


# --- Runtime layer -----------------------------------------------------------


class LifecycleError(DfgError):
    """A lifecycle call was made out of order (running before start, restarting)."""


class NodeContractError(DfgError):
    """A node's ``run`` returned something the output contract does not allow."""


class NodeSetupError(DfgError):
    """A node's ``setup`` raised. The graph does not start."""


class NodeRunError(DfgError):
    """A node's ``run`` raised under the ``stop`` error policy."""


class EdgeOverflowError(DfgError):
    """A bounded edge overflowed under the ``error`` overflow policy."""
