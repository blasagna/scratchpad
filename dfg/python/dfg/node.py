"""The node contract: three lifecycle calls, and zero-or-more outputs.

A node performs some computation on named inputs and produces named outputs. It
is initialized with parameters, may keep private state, and gets exactly three
lifecycle calls: ``setup`` once before the first ``run``, ``run`` per firing, and
``teardown`` once after the last ``run``.

A node is written in one of two forms. The **typed** form names its parameters and
ports as Python identifiers, so a type checker sees them::

    class Decimate(Node):
        factor: int = 2                         # a parameter, with its default

        class Out(NamedTuple):
            output: Emit[Any]                   # an output port

        def run(self, *, inp: In[Any] = ()) -> Out:   # an input port
            return self.Out(output=tuple(kept))

The **declared** form spells the same things as data, which is what a node needs
when its ports are not known until it has its parameters::

    class Splitter(Node):
        INPUTS = (PortSpec("inp"),)
        PARAMS: ClassVar[Mapping[str, Any]] = {"ways": 2}

        @classmethod
        def output_ports(cls, params): ...      # ports depend on a parameter

        def run(self, inputs: Inputs) -> Outputs: ...

Neither is deprecated, and they are chosen **per axis**: parameters, inputs, and
outputs are detected independently, so the splitter above may still take a typed
``ways: int = 2``. The declared form has to stay, because a signature cannot
express ports that depend on parameters, and :meth:`dfg.registry.Registry.
register_factory` has no class to read at all.

The framework calls :meth:`Node.invoke`, which is where the two forms meet and the
only place that has to know which one an author wrote. Nothing wraps or replaces the
``run`` an author wrote, so a traceback points at the node and ``super().run(...)``
from an override is an ordinary call.

Three decisions here are contract, not convenience:

**Ports come from the class.** A blueprint is validated before any node is
constructed, so the port sets have to be answerable without an instance. Whichever
form an author writes, ``INPUTS``/``OUTPUTS`` are settled when the *class* is
created, and :meth:`Node.input_ports`/:meth:`Node.output_ports` remain the escape
hatch for a node whose ports depend on its parameters (an N-way splitter, a
columnar node following a schema).

**``run`` produces zero or more messages per output port.** One-in-one-out is the
common case, not the contract: decimation emits nothing on most invocations and
framing audio into overlapping windows emits several. A mapping and an ``Out``
NamedTuple are two spellings of that one shape -- a field is a tuple of messages,
not a message. A convenience form that accepted one message per port would
immediately produce nodes that only work one way, so returning a bare message is
an error with a message telling the author to wrap it.

**A payload type is not a type tag.** The annotation on a port says what the
payload is, for a type checker. :class:`dfg.ports.Port` says what the *wire* is,
for validation comparing the two ends of an edge. They cannot be the same thing: a
tag is a free-form string that has to survive JSON and mean the same to a port in
another language, and a Python type does neither.
"""

from __future__ import annotations

import dataclasses
import inspect
from abc import ABC, abstractmethod
from collections.abc import Iterable, Mapping, Sequence
from types import MappingProxyType
from typing import Any, ClassVar, Final, get_type_hints

from dfg.errors import ImmutableParamError, NodeContractError, ParamError
from dfg.message import Message
from dfg.ports import Port, PortSpec, is_reserved_name, is_valid_name


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

type Outputs = Mapping[str, Sequence[Message[Any]]] | tuple[Any, ...] | None
"""What ``run`` returns. ``None``, ``{}``, and ``{"output": ()}`` all mean nothing.

The tuple arm is a typed node's ``Out`` NamedTuple, which is a mapping of port name
to zero or more messages by another spelling -- :func:`normalize_outputs` turns one
into the other.
"""

type In[T] = tuple[Message[T], ...]
"""One input port's messages for this firing, as a typed ``run`` receives them.

Always defaults to ``()``: a readiness rule hands over only the ports that had
messages, so a port that had none is simply absent.
"""

type Emit[T] = tuple[Message[T], ...]
"""One output port's messages from this firing, as an ``Out`` field holds them.

The same type as :data:`In` -- deliberately, since a message does not change shape
by crossing an edge. The two names say which end you are reading.
"""


class Node(ABC):
    """A unit of computation with named ports, parameters, and private state.

    Subclasses declare their parameters and ports in either form described in the
    module docstring, then implement :meth:`run`. Registering the class under a
    short, stable type name is what lets a deserialized blueprint instantiate it.
    """

    INPUTS: ClassVar[tuple[PortSpec, ...]] = ()
    """Input ports, in the order an author wrote them.

    Derived from a typed ``run``'s keyword-only parameters, or written directly.
    """

    OUTPUTS: ClassVar[tuple[PortSpec, ...]] = ()
    """Output ports. Publishing walks these in *this* order.

    Derived from the nested ``Out`` NamedTuple's fields, or written directly.
    """

    PARAMS: ClassVar[Mapping[str, Any]] = {}
    """Parameter name to default value. Use :data:`REQUIRED` for no default.

    Derived from annotated class attributes, or written directly.
    """

    MUTABLE_PARAMS: ClassVar[frozenset[str]] = frozenset()
    """Names the control plane may change while the graph runs.

    Parameters are immutable by default. A name listed here can be changed
    *between* ``run`` invocations, never during one, so a node author never has to
    reason about locking.
    """

    _TYPED_PARAMS: ClassVar[bool] = False
    """Whether :attr:`PARAMS` was derived from annotated class attributes.

    When true, a parameter is a real instance attribute and ``self._params`` is
    ``None``; when false, the values live in ``self._params``.
    """

    _TYPED_RUN: ClassVar[bool] = False
    """Whether :meth:`run` takes one keyword argument per input port."""

    def __init_subclass__(cls, **kwargs: Any) -> None:
        """Settle this class's ports and parameters, whichever form was written.

        Runs at *class* creation, which is what keeps the "validate before you
        construct" contract true: by the time a registry describes this type, the
        three class attributes are already final and no instance has been built.

        Raises:
            NodeContractError: If a class mixes both forms on one axis, or declares
                something that cannot be a port or a parameter.
        """
        super().__init_subclass__(**kwargs)
        _derive_params(cls)
        _derive_inputs(cls)
        _derive_outputs(cls)

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
        # One resolution path for both forms, so the two ParamError messages above
        # are the only ones an author ever sees. Typed parameters then become real
        # attributes -- `self.factor`, which a type checker knows the type of.
        self._params: dict[str, Any] | None
        if type(self)._TYPED_PARAMS:
            for name, value in resolved.items():
                setattr(self, name, value)
            self._params = None
        else:
            self._params = resolved
        post_init = getattr(self, "__post_init__", None)
        if post_init is not None:
            post_init()

    @property
    def params(self) -> Mapping[str, Any]:
        """The resolved parameters, as a read-only mapping.

        For a declared-form node this is a live view; for a typed one it is a
        snapshot taken from the attributes. Re-read it after a parameter change
        rather than holding it across one.
        """
        if self._params is None:
            return MappingProxyType({n: getattr(self, n) for n in type(self).PARAMS})
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
    def run(self, *args: Any, **kwargs: Any) -> Any:
        """Consume this firing's inputs and produce outputs. Written in one of two
        forms, which is what the deliberately gradual signature above allows.

        Typed -- one keyword-only parameter per input port, each defaulting to
        ``()``, returning this class's ``Out`` NamedTuple or ``None``::

            def run(self, *, inp: In[float] = ()) -> Out:
                return self.Out(output=tuple(kept))

        Declared -- one positional mapping of the ports that had messages,
        returning a mapping of port name to zero or more messages, or ``None``::

            def run(self, inputs: Inputs) -> Outputs:
                return {"output": kept}

        Either way a port carries **zero or more** messages, never exactly one, and
        sample timestamps are propagated by convention -- see
        :meth:`dfg.message.Message.with_payload`.

        The framework calls :meth:`invoke`, never this directly, so an author is
        free to call a node's ``run`` in whichever form they wrote it.
        """

    def invoke(self, inputs: Inputs) -> Outputs:
        """Call :meth:`run` in whichever form this class declared it.

        The framework's single entry point into a node's computation. Node authors
        neither call nor override this; it exists so that the scheduler has one
        calling convention while an author has two ways to write one.

        Args:
            inputs: Port name to a tuple of one or more messages, containing only
                the ports that had messages available for this firing.
        """
        # `run` is genuinely two different signatures here, which is the one thing
        # a checker cannot follow -- hence the deliberate widening.
        run: Any = self.run
        if type(self)._TYPED_RUN:
            return run(**inputs)
        return run(inputs)

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
        if self._params is None:
            for name, value in changes.items():
                setattr(self, name, value)
        else:
            self._params.update(changes)
        self.on_params_changed(dict(changes))


_POSITIONAL_KINDS: Final = frozenset(
    {
        inspect.Parameter.POSITIONAL_ONLY,
        inspect.Parameter.POSITIONAL_OR_KEYWORD,
        inspect.Parameter.VAR_POSITIONAL,
    }
)


def _derive_params(cls: type[Node]) -> None:
    """Settle ``PARAMS`` from annotated class attributes, if there are any.

    ``dataclasses`` is used for field *collection* only -- ``init=False``, because
    the one constructor stays :meth:`Node.__init__` and its ``ParamError`` messages.
    What it brings is the fiddly part: ``ClassVar`` exclusion that works on string
    annotations, inheritance, and its refusal of mutable defaults.
    """
    dataclasses.dataclass(init=False, kw_only=True, eq=False, repr=False)(cls)
    # dataclasses.fields wants a DataclassInstance; cls became one on the line
    # above, which is a fact no checker can follow.
    fields = dataclasses.fields(cls)  # pyrefly: ignore[bad-argument-type]
    if not fields:
        return
    if "PARAMS" in cls.__dict__:
        raise NodeContractError(
            f"{cls.__name__} declares both annotated parameter(s) "
            f"{[f.name for f in fields]} and a PARAMS mapping; a node uses one form "
            f"or the other for its parameters"
        )
    derived: dict[str, Any] = {}
    for field in fields:
        if field.name.startswith("_"):
            raise NodeContractError(
                f"{cls.__name__} annotates {field.name!r} at class level, which makes "
                f"it a parameter, and a parameter is public; assign private state in "
                f"setup() instead, or mark it ClassVar if it is a constant"
            )
        if field.default_factory is not dataclasses.MISSING:
            raise NodeContractError(
                f"{cls.__name__}.{field.name} uses default_factory; a parameter's "
                f"default has to be a value the registry can report without building "
                f"a node, so it must be a plain immutable value"
            )
        derived[field.name] = (
            REQUIRED if field.default is dataclasses.MISSING else field.default
        )
    cls.PARAMS = derived
    cls._TYPED_PARAMS = True


def _derive_inputs(cls: type[Node]) -> None:
    """Settle ``INPUTS`` from ``run``'s keyword-only parameters, if it has any.

    The two forms are told apart by shape rather than by a marker: the declared form
    takes one positional mapping, so *any* positional parameter after ``self`` means
    the author wrote that one.
    """
    run = getattr(cls, "run", None)
    if run is None:
        return
    parameters = list(inspect.signature(run).parameters.values())[1:]  # drop self
    if any(p.kind in _POSITIONAL_KINDS for p in parameters):
        return
    cls._TYPED_RUN = True
    if "INPUTS" in cls.__dict__:
        raise NodeContractError(
            f"{cls.__name__} declares INPUTS and a typed run(); a typed run's "
            f"keyword-only parameters are its input ports, so INPUTS would be a "
            f"second answer to the same question"
        )
    tags = _port_tags(run, cls)
    ports: list[PortSpec] = []
    for parameter in parameters:
        if parameter.kind is inspect.Parameter.VAR_KEYWORD:
            raise NodeContractError(
                f"{cls.__name__}.run declares **{parameter.name}; a blueprint is "
                f"validated before any node is built, so the ports have to be "
                f"readable from the class -- name one parameter per input port"
            )
        if parameter.default != ():
            raise NodeContractError(
                f"{cls.__name__}.run input port {parameter.name!r} must default to "
                f"(); a readiness rule hands over only the ports that had messages, "
                f"so a port with nothing available this firing is simply absent"
            )
        _check_port_name(cls, parameter.name, "input")
        tag = tags.get(parameter.name) or Port()
        ports.append(PortSpec(parameter.name, tag.type_tag, tag.description))
    cls.INPUTS = tuple(ports)


def _derive_outputs(cls: type[Node]) -> None:
    """Settle ``OUTPUTS`` from the nested ``Out`` NamedTuple, if there is one.

    Read from the class object rather than from ``run``'s return annotation, so that
    a subclass overriding ``run`` may annotate it ``-> Base.Out`` -- a class body's
    scope is not inherited, so a bare ``-> Out`` would not resolve there.
    """
    out = getattr(cls, "Out", None)
    if not (
        isinstance(out, type) and issubclass(out, tuple) and hasattr(out, "_fields")
    ):
        return
    if "OUTPUTS" in cls.__dict__:
        raise NodeContractError(
            f"{cls.__name__} declares OUTPUTS and an Out NamedTuple; a node uses one "
            f"form or the other for its output ports"
        )
    tags = _port_tags(out, cls)
    ports: list[PortSpec] = []
    for name in out._fields:  # field order is declared port order
        _check_port_name(cls, name, "output")
        tag = tags.get(name) or Port()
        ports.append(PortSpec(name, tag.type_tag, tag.description))
    cls.OUTPUTS = tuple(ports)


def _check_port_name(cls: type[Node], name: str, direction: str) -> None:
    """Reject a port name at class creation, where the class can still be named."""
    if is_reserved_name(name):
        raise NodeContractError(
            f"{cls.__name__} declares {direction} port {name!r}, and names starting "
            f"with '__' belong to the framework"
        )
    if not is_valid_name(name):
        raise NodeContractError(
            f"{cls.__name__} declares {direction} port {name!r}, which is not a legal "
            f"port name"
        )


def _port_tags(obj: object, cls: type[Node]) -> dict[str, Port]:
    """Read each name's :class:`~dfg.ports.Port` metadata out of ``Annotated[...]``.

    Resolving annotations needs the class's own namespace as locals, because a port
    may be annotated with a type defined in the class body. Names that cannot be
    resolved are not fatal on their own -- node classes get defined inside test
    functions, where a local name genuinely is not reachable from here -- but they
    are fatal if a ``Port(...)`` tag would be silently dropped by giving up.
    """
    localns: dict[str, Any] = {}
    for base in reversed(cls.__mro__):
        localns.update(vars(base))
    try:
        hints = get_type_hints(obj, localns=localns, include_extras=True)
    except NameError as exc:
        raw = getattr(obj, "__annotations__", {})
        if any("Port(" in str(value) for value in raw.values()):
            raise NodeContractError(
                f"{cls.__name__}: a Port(...) tag is read when the class is created, "
                f"so every name its annotation mentions has to be defined by then "
                f"({exc})"
            ) from exc
        return {}
    tags: dict[str, Port] = {}
    for name, hint in hints.items():
        for meta in getattr(hint, "__metadata__", ()):
            if isinstance(meta, Port):
                tags[name] = meta
    return tags


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
    if isinstance(outputs, tuple) and hasattr(type(outputs), "_fields"):
        # A typed node's Out: the same port-name-to-messages shape, spelled as
        # fields. Everything below is then one code path for both forms.
        outputs = outputs._asdict()  # pyrefly: ignore[missing-attribute]
    if isinstance(outputs, Message):
        raise NodeContractError(
            f"{where}.run returned a bare Message; run returns zero or more "
            f"messages per output port -- wrap it, e.g. "
            f'return {{"{declared[0].name if declared else "output"}": [msg]}}'
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
