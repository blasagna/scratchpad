"""The node contract: three lifecycle calls, and zero-or-more outputs.

A node performs some computation on named inputs and produces named outputs. It
is initialized with parameters, may keep private state, and gets exactly three
lifecycle calls: ``setup`` once before the first ``run``, ``run`` per firing, and
``teardown`` once after the last ``run``.

Two decisions here are contract, not convenience:

**Ports come from the class.** A blueprint is validated before any node is
constructed, so the port sets have to be answerable without an instance. They are
``INPUTS``/``OUTPUTS`` class attributes, with :meth:`Node.input_ports` and
:meth:`Node.output_ports` classmethods as the escape hatch for a node whose ports
depend on its parameters (an N-way splitter, a columnar node following a schema).

**``run`` returns a mapping of port name to zero or more messages.**
One-in-one-out is the common case, not the contract: decimation emits nothing on
most invocations and framing audio into overlapping windows emits several. A
convenience form that accepted one message per port would immediately produce
nodes that only work one way, so returning a bare message is an error with a
message telling the author to wrap it.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Iterable, Mapping, Sequence
from types import MappingProxyType
from typing import Any, ClassVar, Final

from dfg.errors import ImmutableParamError, NodeContractError, ParamError
from dfg.message import Message
from dfg.ports import PortSpec


class _Required:
    """Sentinel marking a parameter with no default. See :data:`REQUIRED`."""

    __slots__ = ()

    def __repr__(self) -> str:
        return "REQUIRED"


REQUIRED: Final = _Required()
"""Use as a ``PARAMS`` default to mean "the author must supply this"."""


type Inputs = Mapping[str, tuple[Message[Any], ...]]
"""What ``run`` receives: only the ports that had messages this firing.

A port absent from the mapping had nothing available, so a node with ``any``
readiness writes ``inputs.get("imu", ())``. Values are tuples because a readiness
rule may hand a node several messages at once -- which is what makes a "fire when
512 samples are buffered" rule usable for audio.
"""

type Outputs = Mapping[str, Sequence[Message[Any]]] | None
"""What ``run`` returns. ``None``, ``{}``, and ``{"out": ()}`` all mean nothing."""


class Node(ABC):
    """A unit of computation with named ports, parameters, and private state.

    Subclasses declare :attr:`INPUTS`, :attr:`OUTPUTS`, and :attr:`PARAMS`, then
    implement :meth:`run`. Registering the class under a short, stable type name
    is what lets a deserialized blueprint instantiate it.
    """

    INPUTS: ClassVar[tuple[PortSpec, ...]] = ()
    """Declared input ports, in the order an author wrote them."""

    OUTPUTS: ClassVar[tuple[PortSpec, ...]] = ()
    """Declared output ports. Publishing walks these in *this* order."""

    PARAMS: ClassVar[Mapping[str, Any]] = {}
    """Parameter name to default value. Use :data:`REQUIRED` for no default."""

    MUTABLE_PARAMS: ClassVar[frozenset[str]] = frozenset()
    """Names the control plane may change while the graph runs.

    Parameters are immutable by default. A name listed here can be changed
    *between* ``run`` invocations, never during one, so a node author never has to
    reason about locking.
    """

    def __init__(self, **params: Any) -> None:
        """Resolve ``params`` against :attr:`PARAMS`.

        Raises:
            ParamError: On an unknown parameter name, or a required parameter with
                no value supplied.
        """
        unknown = sorted(set(params) - set(self.PARAMS))
        if unknown:
            known = sorted(self.PARAMS)
            raise ParamError(
                f"{type(self).__name__} got unknown parameter(s) {unknown}; "
                f"declared parameters are {known}"
            )
        resolved: dict[str, Any] = {}
        missing: list[str] = []
        for name, default in self.PARAMS.items():
            if name in params:
                resolved[name] = params[name]
            elif isinstance(default, _Required):
                missing.append(name)
            else:
                resolved[name] = default
        if missing:
            raise ParamError(
                f"{type(self).__name__} is missing required parameter(s) "
                f"{sorted(missing)}"
            )
        self._params = resolved

    @property
    def params(self) -> Mapping[str, Any]:
        """The resolved parameters, as a read-only view."""
        return MappingProxyType(self._params)

    @classmethod
    def input_ports(cls, params: Mapping[str, Any]) -> tuple[PortSpec, ...]:
        """Input ports for a node built with ``params``, without constructing one.

        Defaults to :attr:`INPUTS`. Override for parameter-dependent ports.
        """
        del params
        return cls.INPUTS

    @classmethod
    def output_ports(cls, params: Mapping[str, Any]) -> tuple[PortSpec, ...]:
        """Output ports for a node built with ``params``, without constructing one.

        Defaults to :attr:`OUTPUTS`. Override for parameter-dependent ports.
        """
        del params
        return cls.OUTPUTS

    def setup(self) -> None:
        """Acquire whatever this node needs. Called exactly once before ``run``.

        If this raises, the graph fails to start and this node does **not** get a
        ``teardown`` -- so acquire in an order that leaves nothing dangling.
        """

    @abstractmethod
    def run(self, inputs: Inputs) -> Outputs:
        """Consume ``inputs`` and produce outputs.

        Args:
            inputs: Port name to a tuple of one or more messages, containing only
                the ports that had messages available for this firing.

        Returns:
            A mapping of output port name to zero or more messages, or ``None``.
            Sample timestamps are propagated by convention -- see
            :meth:`dfg.message.Message.with_payload`.
        """

    def teardown(self) -> None:
        """Release whatever ``setup`` acquired. Called exactly once after ``run``.

        Runs on a clean stop *and* on an error stop. It is the only place a node
        may assume it will be given.
        """

    def on_params_changed(self, changes: Mapping[str, Any]) -> None:
        """React to a live parameter change, applied between ``run`` calls.

        Args:
            changes: Only the names that changed. :attr:`params` already reflects
                them by the time this is called.
        """

    def _apply_param_changes(self, changes: Mapping[str, Any]) -> None:
        """Apply a control-plane parameter change. Called by the scheduler only.

        Raises:
            ImmutableParamError: If a name is not in :attr:`MUTABLE_PARAMS`.
            ParamError: If a name is not a declared parameter at all.
        """
        for name in changes:
            if name not in self.PARAMS:
                raise ParamError(
                    f"{type(self).__name__} has no parameter {name!r}; "
                    f"declared parameters are {sorted(self.PARAMS)}"
                )
            if name not in self.MUTABLE_PARAMS:
                raise ImmutableParamError(
                    f"{type(self).__name__} parameter {name!r} is immutable; "
                    f"list it in MUTABLE_PARAMS to allow live changes"
                )
        self._params.update(changes)
        self.on_params_changed(dict(changes))


def normalize_outputs(
    outputs: Outputs,
    declared: tuple[PortSpec, ...],
    *,
    where: str,
) -> dict[str, tuple[Message[Any], ...]]:
    """Check a ``run`` return value against the output contract and normalize it.

    Args:
        outputs: Whatever ``run`` returned.
        declared: The node's declared output ports.
        where: The qualified node ID, for the error message.

    Returns:
        Port name to a tuple of messages, containing only ports that produced at
        least one message, in *declared* port order -- the iteration order of the
        mapping a node returned is never trusted, because it would make publishing
        order depend on how an author happened to build a dict.

    Raises:
        NodeContractError: If a bare message was returned instead of a mapping, if
            a port name is not declared, or if a value is not a sequence of
            messages.
    """
    if outputs is None:
        return {}
    if isinstance(outputs, Message):
        raise NodeContractError(
            f"{where}.run returned a bare Message; run returns a mapping of "
            f"output port name to zero or more messages -- wrap it, e.g. "
            f'return {{"{declared[0].name if declared else "out"}": [msg]}}'
        )
    if not isinstance(outputs, Mapping):
        raise NodeContractError(
            f"{where}.run returned {type(outputs).__name__}; run returns a "
            f"mapping of output port name to zero or more messages, or None"
        )
    declared_names = [port.name for port in declared]
    unknown = sorted(set(outputs) - set(declared_names))
    if unknown:
        raise NodeContractError(
            f"{where}.run produced unknown output port(s) {unknown}; "
            f"declared output ports are {declared_names}"
        )
    normalized: dict[str, tuple[Message[Any], ...]] = {}
    for name in declared_names:
        if name not in outputs:
            continue
        messages = _as_message_tuple(outputs[name], where=where, port=name)
        if messages:
            normalized[name] = messages
    return normalized


def _as_message_tuple(
    value: object, *, where: str, port: str
) -> tuple[Message[Any], ...]:
    """Coerce one port's returned value to a tuple of messages, or raise."""
    if isinstance(value, Message):
        raise NodeContractError(
            f"{where}.run returned a bare Message on port {port!r}; a port's "
            f"value is zero or more messages -- wrap it in a list"
        )
    if isinstance(value, (str, bytes)) or not isinstance(value, Iterable):
        raise NodeContractError(
            f"{where}.run returned {type(value).__name__} on port {port!r}; "
            f"a port's value is a sequence of Messages"
        )
    messages = tuple(value)
    for item in messages:
        if not isinstance(item, Message):
            raise NodeContractError(
                f"{where}.run returned a {type(item).__name__} on port {port!r}; "
                f"every element must be a Message (payload plus timestamp)"
            )
    return messages
