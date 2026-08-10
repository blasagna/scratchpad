"""The typed node form: what a class declares, and what gets derived from it.

Everything here is about class *creation*. A blueprint is validated before any node
is constructed, so a typed node's ports and parameters have to be settled by the
time the class body finishes -- these tests are what pins that down.
"""

from __future__ import annotations

import unittest
from typing import Annotated, Any, ClassVar, NamedTuple

from dfg.errors import ImmutableParamError, NodeContractError, ParamError
from dfg.message import Message
from dfg.node import Emit, In, Node, _Required, normalize_outputs
from dfg.ports import Port, PortSpec


def msg(payload: Any, timestamp: int = 0) -> Message[Any]:
    return Message(payload, timestamp)


class Doubler(Node):
    """The reference typed node: one param, one input port, one output port."""

    gain: int = 2

    class Out(NamedTuple):
        output: Emit[int]

    def run(self, *, input: In[int] = ()) -> Out:
        return self.Out(
            output=tuple(m.with_payload(m.payload * self.gain) for m in input)
        )


class TestParamDerivation(unittest.TestCase):
    def test_annotated_attributes_become_params_with_their_defaults(self):
        self.assertEqual(Doubler.PARAMS, {"gain": 2})
        self.assertTrue(Doubler._TYPED_PARAMS)

    def test_declaration_order_is_preserved(self):
        class Many(Node):
            first: int = 1
            second: str = "s"
            third: float = 0.5

            def run(self, *, input: In[Any] = ()) -> None: ...

        self.assertEqual(list(Many.PARAMS), ["first", "second", "third"])

    def test_an_attribute_with_no_default_is_required(self):
        class NeedsSize(Node):
            size: int
            hop: int = 1

            def run(self, *, input: In[Any] = ()) -> None: ...

        # The exact predicate validate._check_params uses.
        self.assertIsInstance(NeedsSize.PARAMS["size"], _Required)
        self.assertEqual(NeedsSize.PARAMS["hop"], 1)

    def test_a_required_param_is_still_keyword_only(self):
        class NeedsSize(Node):
            size: int

            def run(self, *, input: In[Any] = ()) -> None: ...

        with self.assertRaises(ParamError) as caught:
            NeedsSize()
        self.assertIn("size", str(caught.exception))

    def test_a_classvar_is_a_constant_not_a_param(self):
        class WithConstant(Node):
            WEIGHTS: ClassVar[tuple[float, ...]] = (0.2, 0.7, 0.1)
            gain: float = 1.0

            def run(self, *, input: In[Any] = ()) -> None: ...

        self.assertEqual(WithConstant.PARAMS, {"gain": 1.0})

    def test_params_are_inherited(self):
        class Child(Doubler):
            def setup(self) -> None: ...

        self.assertEqual(Child.PARAMS, {"gain": 2})
        self.assertEqual(Child(gain=5).gain, 5)

    def test_a_subclass_may_add_a_param(self):
        class Child(Doubler):
            offset: int = 0

            def run(self, *, input: In[int] = ()) -> Doubler.Out:
                return self.Out(output=())

        self.assertEqual(Child.PARAMS, {"gain": 2, "offset": 0})


class TestParamAccess(unittest.TestCase):
    def test_a_param_is_a_real_attribute(self):
        self.assertEqual(Doubler(gain=7).gain, 7)

    def test_params_mapping_agrees_with_the_attributes(self):
        node = Doubler(gain=7)
        self.assertEqual(dict(node.params), {"gain": 7})

    def test_the_params_mapping_is_read_only(self):
        with self.assertRaises(TypeError):
            # A read-only mapping refusing the write is the point of the test.
            Doubler().params["gain"] = 3  # pyrefly: ignore[unsupported-operation]

    def test_post_init_validates_and_raises_from_the_constructor(self):
        class Fussy(Node):
            factor: int = 1

            def __post_init__(self) -> None:
                if self.factor < 1:
                    raise ParamError(f"factor must be at least 1, got {self.factor}")

            def run(self, *, input: In[Any] = ()) -> None: ...

        self.assertEqual(Fussy(factor=4).factor, 4)
        with self.assertRaises(ParamError):
            Fussy(factor=0)

    def test_a_live_change_sets_the_attribute_and_fires_the_hook(self):
        class Tunable(Node):
            gain: int = 1
            MUTABLE_PARAMS = frozenset({"gain"})

            def setup(self) -> None:
                self.seen: list[dict[str, Any]] = []

            def on_params_changed(self, changes):
                self.seen.append(dict(changes))

            def run(self, *, input: In[Any] = ()) -> None: ...

        node = Tunable()
        node.setup()
        node._apply_param_changes({"gain": 9})
        self.assertEqual(node.gain, 9)
        self.assertEqual(node.params["gain"], 9)
        self.assertEqual(node.seen, [{"gain": 9}])

    def test_an_immutable_param_still_refuses_a_live_change(self):
        with self.assertRaises(ImmutableParamError):
            Doubler()._apply_param_changes({"gain": 3})


class TestPortDerivation(unittest.TestCase):
    def test_keyword_only_run_parameters_become_input_ports(self):
        self.assertEqual(Doubler.INPUTS, (PortSpec("input"),))
        self.assertTrue(Doubler._TYPED_RUN)

    def test_out_fields_become_output_ports_in_field_order(self):
        class Split(Node):
            class Out(NamedTuple):
                gravity: Emit[float]
                linear: Emit[float]

            def run(self, *, input: In[float] = ()) -> Out:
                return self.Out(gravity=(), linear=())

        self.assertEqual([port.name for port in Split.OUTPUTS], ["gravity", "linear"])

    def test_input_ports_keep_signature_order(self):
        class Pair(Node):
            class Out(NamedTuple):
                output: Emit[Any]

            def run(self, *, slow: In[Any] = (), fast: In[Any] = ()) -> Out:
                return self.Out(output=())

        self.assertEqual([port.name for port in Pair.INPUTS], ["slow", "fast"])

    def test_a_port_annotation_carries_its_type_tag_and_description(self):
        class Tagged(Node):
            class Out(NamedTuple):
                output: Annotated[Emit[Any], Port("RecordBatch", "one batch")]

            def run(self, *, input: Annotated[In[Any], Port("ImuSample")] = ()) -> Out:
                return self.Out(output=())

        self.assertEqual(Tagged.INPUTS, (PortSpec("input", "ImuSample"),))
        self.assertEqual(
            Tagged.OUTPUTS, (PortSpec("output", "RecordBatch", "one batch"),)
        )

    def test_an_untagged_port_is_untyped(self):
        self.assertIsNone(Doubler.INPUTS[0].type_tag)
        self.assertEqual(Doubler.INPUTS[0].description, "")

    def test_a_run_returning_none_declares_no_output_ports(self):
        class Sink(Node):
            def run(self, *, input: In[Any] = ()) -> None: ...

        self.assertEqual(Sink.OUTPUTS, ())

    def test_ports_are_inherited_unchanged_by_a_lifecycle_only_subclass(self):
        class Child(Doubler):
            def teardown(self) -> None: ...

        self.assertEqual(Child.INPUTS, Doubler.INPUTS)
        self.assertEqual(Child.OUTPUTS, Doubler.OUTPUTS)

    def test_a_subclass_overriding_run_rederives_its_own_ports(self):
        # Both suppressions are the honest report of something true: a node that
        # redeclares its ports is not a *subtype* of the one it inherits from, it is
        # a different node type reusing its code. Inheritance is available for that,
        # and a checker is right to say the shapes differ.
        class Child(Doubler):
            class Out(NamedTuple):  # pyrefly: ignore[bad-override]
                left: Emit[int]
                right: Emit[int]

            def run(  # pyrefly: ignore[bad-override]
                self, *, a: In[int] = (), b: In[int] = ()
            ) -> Out:
                return self.Out(left=(), right=())

        self.assertEqual([p.name for p in Child.INPUTS], ["a", "b"])
        self.assertEqual([p.name for p in Child.OUTPUTS], ["left", "right"])
        self.assertEqual([p.name for p in Doubler.INPUTS], ["input"])

    def test_an_overriding_run_may_annotate_the_bases_out(self):
        # A class body's scope is not inherited, so `-> Out` would not resolve in a
        # subclass. `-> Doubler.Out` does, and OUTPUTS comes from the class anyway.
        class Child(Doubler):
            def run(self, *, input: In[int] = ()) -> Doubler.Out:
                return self.Out(output=(msg(1),))

        self.assertEqual(Child.OUTPUTS, Doubler.OUTPUTS)
        self.assertEqual(Child().run(input=()).output, (msg(1),))


class TestFormDetection(unittest.TestCase):
    def test_a_positional_parameter_means_the_declared_form(self):
        class Declared(Node):
            INPUTS = (PortSpec("input"),)
            OUTPUTS = (PortSpec("output"),)

            def run(self, inputs):
                return {"output": list(inputs.get("input", ()))}

        self.assertFalse(Declared._TYPED_RUN)
        self.assertFalse(Declared._TYPED_PARAMS)
        self.assertEqual(Declared.INPUTS, (PortSpec("input"),))

    def test_the_axes_are_independent(self):
        # Typed params, a declared run, and parameter-dependent output ports: the
        # combination the declared form has to keep existing for.
        class Splitter(Node):
            INPUTS = (PortSpec("input"),)
            ways: int = 2

            @classmethod
            def output_ports(cls, params):
                return tuple(PortSpec(f"out{i}") for i in range(params["ways"]))

            def run(self, inputs):
                return None

        self.assertTrue(Splitter._TYPED_PARAMS)
        self.assertFalse(Splitter._TYPED_RUN)
        self.assertEqual(Splitter.PARAMS, {"ways": 2})
        self.assertEqual(
            [p.name for p in Splitter.output_ports({"ways": 3})],
            ["out0", "out1", "out2"],
        )

    def test_invoke_routes_each_form(self):
        class Declared(Node):
            INPUTS = (PortSpec("input"),)
            OUTPUTS = (PortSpec("output"),)

            def run(self, inputs):
                return {"output": list(inputs.get("input", ()))}

        self.assertEqual(Declared().invoke({"input": (msg(1),)}), {"output": [msg(1)]})
        self.assertEqual(
            Doubler().invoke({"input": (msg(1),)}), Doubler.Out(output=(msg(2),))
        )

    def test_an_absent_port_arrives_as_the_empty_default(self):
        seen: list[tuple[Any, ...]] = []

        class Watcher(Node):
            def run(self, *, input: In[Any] = ()) -> None:
                seen.append(input)

        Watcher().invoke({})
        self.assertEqual(seen, [()])

    def test_run_is_the_function_the_author_wrote(self):
        # Nothing is wrapped, so a traceback points at the node and `super().run()`
        # from an override is an ordinary call.
        self.assertIs(Doubler.run, Doubler.__dict__["run"])


class TestClassCreationErrors(unittest.TestCase):
    def test_annotated_params_and_a_params_mapping_together(self):
        with self.assertRaises(NodeContractError) as caught:

            class Both(Node):
                PARAMS = {"gain": 1}
                offset: int = 0

                def run(self, *, input: In[Any] = ()) -> None: ...

        self.assertIn("one form or the other", str(caught.exception))

    def test_a_typed_run_and_an_inputs_declaration_together(self):
        with self.assertRaises(NodeContractError) as caught:

            class Both(Node):
                INPUTS = (PortSpec("input"),)

                def run(self, *, input: In[Any] = ()) -> None: ...

        self.assertIn("INPUTS", str(caught.exception))

    def test_an_out_namedtuple_and_an_outputs_declaration_together(self):
        with self.assertRaises(NodeContractError) as caught:

            class Both(Node):
                OUTPUTS = (PortSpec("output"),)

                class Out(NamedTuple):
                    output: Emit[Any]

                def run(self, *, input: In[Any] = ()) -> Out:
                    return self.Out(output=())

        self.assertIn("one form or the other", str(caught.exception))

    def test_an_input_port_without_an_empty_default(self):
        with self.assertRaises(NodeContractError) as caught:

            class NoDefault(Node):
                def run(self, *, input: In[Any]) -> None: ...

        self.assertIn("must default to ()", str(caught.exception))

    def test_var_keyword_on_a_typed_run(self):
        with self.assertRaises(NodeContractError) as caught:

            class Splat(Node):
                def run(self, **ports: In[Any]) -> None: ...

        self.assertIn("one parameter per input port", str(caught.exception))

    def test_a_reserved_input_port_name(self):
        with self.assertRaises(NodeContractError) as caught:

            class Sneaky(Node):
                def run(self, *, __error__: In[Any] = ()) -> None: ...

        self.assertIn("belong to the framework", str(caught.exception))

    def test_a_reserved_output_port_name_is_refused_by_namedtuple_first(self):
        # The framework's reserved-prefix check is unreachable here: NamedTuple
        # refuses an underscore-prefixed field itself, and says so just as clearly.
        with self.assertRaises(ValueError) as caught:

            class Sneaky(Node):
                class Out(NamedTuple):
                    __error__: Emit[Any]  # pyrefly: ignore[bad-class-definition]

                def run(self, *, input: In[Any] = ()) -> Out: ...

        self.assertIn("cannot start with an underscore", str(caught.exception))

    def test_an_output_port_name_that_is_not_a_legal_port_name(self):
        with self.assertRaises(NodeContractError) as caught:

            class Accented(Node):
                class Out(NamedTuple):
                    café: Emit[Any]  # a Python identifier, but not a port name

                def run(self, *, input: In[Any] = ()) -> Out: ...

        self.assertIn("not a legal port name", str(caught.exception))

    def test_a_private_class_annotation_is_not_a_parameter(self):
        with self.assertRaises(NodeContractError) as caught:

            class Stateful(Node):
                _seen: int = 0

                def run(self, *, input: In[Any] = ()) -> None: ...

        self.assertIn("setup()", str(caught.exception))

    def test_a_mutable_param_default_is_refused(self):
        with self.assertRaises(ValueError):

            class Leaky(Node):
                sink: list[int] = []

                def run(self, *, input: In[Any] = ()) -> None: ...


class TestOutputContractHolds(unittest.TestCase):
    """The zero-or-more rule, checked through the Out spelling of it."""

    def test_an_out_namedtuple_normalizes_to_declared_order(self):
        class Split(Node):
            class Out(NamedTuple):
                first: Emit[int]
                second: Emit[int]

            def run(self, *, input: In[int] = ()) -> Out:
                return self.Out(second=(msg(2),), first=(msg(1),))

        produced = Split().invoke({"input": ()})
        self.assertEqual(
            list(normalize_outputs(produced, Split.OUTPUTS, where="s")),
            ["first", "second"],
        )

    def test_an_empty_port_publishes_nothing(self):
        produced = Doubler().invoke({"input": ()})
        self.assertEqual(normalize_outputs(produced, Doubler.OUTPUTS, where="d"), {})

    def test_a_typed_run_returning_none_means_nothing(self):
        class Quiet(Node):
            class Out(NamedTuple):
                output: Emit[Any]

            def run(self, *, input: In[Any] = ()) -> Out | None:
                return None

        produced = Quiet().invoke({"input": ()})
        self.assertEqual(normalize_outputs(produced, Quiet.OUTPUTS, where="q"), {})

    def test_a_bare_message_is_still_an_error(self):
        class Sloppy(Node):
            class Out(NamedTuple):
                output: Emit[Any]

            def run(self, *, input: In[Any] = ()):
                return msg(1)

        produced = Sloppy().invoke({"input": ()})
        with self.assertRaises(NodeContractError) as caught:
            normalize_outputs(produced, Sloppy.OUTPUTS, where="s")
        self.assertIn("bare Message", str(caught.exception))

    def test_a_message_not_wrapped_in_a_sequence_is_still_an_error(self):
        class Sloppy(Node):
            class Out(NamedTuple):
                output: Emit[Any]

            def run(self, *, input: In[Any] = ()):
                return self.Out(output=msg(1))  # pyrefly: ignore[bad-argument-type]

        produced = Sloppy().invoke({"input": ()})
        with self.assertRaises(NodeContractError) as caught:
            normalize_outputs(produced, Sloppy.OUTPUTS, where="s")
        self.assertIn("wrap it in a list", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
