"""Ports, node parameter resolution, the output contract, and the registry."""

import doctest
import unittest

import helpers
from dfg import registry as registry_module
from dfg.errors import (
    ImmutableParamError,
    NodeContractError,
    ParamError,
    UnknownNodeTypeError,
)
from dfg.message import Message
from dfg.node import REQUIRED, Node, normalize_outputs
from dfg.ports import PortSpec, is_reserved_name, is_valid_name, qualify, topic_of
from dfg.registry import Registry


class TestNames(unittest.TestCase):
    def test_accepts_identifier_like_names(self):
        for name in ("a", "imu_raw", "port2", "A1_b"):
            with self.subTest(name=name):
                self.assertTrue(is_valid_name(name))

    def test_rejects_names_that_would_break_a_dotted_path(self):
        for name in ("", "1a", "a.b", "a-b", "a b", "_a", "a/b"):
            with self.subTest(name=name):
                self.assertFalse(is_valid_name(name))

    def test_reserved_prefix(self):
        self.assertTrue(is_reserved_name("__error__"))
        self.assertFalse(is_reserved_name("error"))

    def test_qualify_and_topic(self):
        self.assertEqual(qualify((), "calib"), "calib")
        self.assertEqual(qualify(("fusion",), "predict"), "fusion.predict")
        self.assertEqual(qualify(("a", "b"), "c"), "a.b.c")
        self.assertEqual(topic_of("fusion.update", "fused"), "fusion.update.fused")


class TestNodeParams(unittest.TestCase):
    def test_defaults_are_applied(self):
        self.assertEqual(helpers.EmitN().params, {"n": 3})

    def test_overrides_win(self):
        self.assertEqual(helpers.EmitN(n=7).params["n"], 7)

    def test_unknown_parameter_is_rejected(self):
        with self.assertRaises(ParamError) as caught:
            helpers.EmitN(nope=1)
        self.assertIn("nope", str(caught.exception))
        self.assertIn("['n']", str(caught.exception))

    def test_missing_required_parameter_is_rejected(self):
        class NeedsOne(Node):
            INPUTS = (PortSpec("input"),)
            PARAMS = {"size": REQUIRED}

            def run(self, inputs):
                return None

        with self.assertRaises(ParamError) as caught:
            NeedsOne()
        self.assertIn("size", str(caught.exception))
        self.assertEqual(NeedsOne(size=4).params["size"], 4)

    def test_params_view_is_read_only(self):
        node = helpers.EmitN()
        with self.assertRaises(TypeError):
            node.params["n"] = 9

    def test_immutable_by_default(self):
        node = helpers.EmitN()
        with self.assertRaises(ImmutableParamError):
            node._apply_param_changes({"n": 9})
        self.assertEqual(node.params["n"], 3)

    def test_mutable_params_opt_in_and_notify(self):
        changes: list[dict] = []
        node = helpers.ParamWatcher(changes=changes)
        node._apply_param_changes({"gain": 4})
        self.assertEqual(node.params["gain"], 4)
        self.assertEqual(changes, [{"gain": 4}])

    def test_changing_an_undeclared_param_is_a_param_error(self):
        node = helpers.ParamWatcher()
        with self.assertRaises(ParamError):
            node._apply_param_changes({"nope": 1})

    def test_ports_are_answerable_from_the_class(self):
        self.assertEqual([p.name for p in helpers.Sum2.input_ports({})], ["a", "b"])
        self.assertEqual([p.name for p in helpers.Sum2.output_ports({})], ["output"])

    def test_parameter_dependent_ports(self):
        # Typed params, a declared run, and parameter-dependent output ports: the
        # combination that keeps the declared form from being deprecated.
        class Splitter(Node):
            INPUTS = (PortSpec("input"),)
            PARAMS = {"ways": 2}

            @classmethod
            def output_ports(cls, params):
                return tuple(PortSpec(f"out{i}") for i in range(params["ways"]))

            def run(self, inputs):
                return None

        self.assertEqual(
            [p.name for p in Splitter.output_ports({"ways": 3})],
            ["out0", "out1", "out2"],
        )


class TestOutputContract(unittest.TestCase):
    declared = (PortSpec("output"), PortSpec("other"))

    def normalize(self, outputs):
        return normalize_outputs(outputs, self.declared, where="n")

    def test_none_and_empty_forms_all_mean_nothing(self):
        for outputs in (None, {}, {"output": ()}, {"output": [], "other": ()}):
            with self.subTest(outputs=outputs):
                self.assertEqual(self.normalize(outputs), {})

    def test_several_messages_keep_their_order(self):
        messages = [Message(i, i) for i in range(3)]
        self.assertEqual(
            self.normalize({"output": messages}), {"output": tuple(messages)}
        )

    def test_ports_come_back_in_declared_order_not_returned_order(self):
        # Publishing order must not depend on how an author happened to build a
        # dict, so the declared order wins.
        result = self.normalize({"other": [Message(1, 1)], "output": [Message(2, 2)]})
        self.assertEqual(list(result), ["output", "other"])

    def test_a_bare_message_is_rejected_with_advice(self):
        with self.assertRaises(NodeContractError) as caught:
            self.normalize(Message(1, 1))
        self.assertIn('return {"output": [msg]}', str(caught.exception))

    def test_a_bare_message_on_a_port_is_rejected(self):
        with self.assertRaises(NodeContractError) as caught:
            self.normalize({"output": Message(1, 1)})
        self.assertIn("wrap it in a list", str(caught.exception))

    def test_an_undeclared_port_is_rejected(self):
        with self.assertRaises(NodeContractError) as caught:
            self.normalize({"nope": [Message(1, 1)]})
        self.assertIn("nope", str(caught.exception))

    def test_a_non_message_element_is_rejected(self):
        with self.assertRaises(NodeContractError) as caught:
            self.normalize({"output": [1, 2]})
        self.assertIn("must be a Message", str(caught.exception))

    def test_a_non_mapping_return_is_rejected(self):
        with self.assertRaises(NodeContractError):
            self.normalize([Message(1, 1)])

    def test_a_string_on_a_port_is_rejected_rather_than_iterated(self):
        with self.assertRaises(NodeContractError):
            self.normalize({"output": "oops"})


class TestRegistry(unittest.TestCase):
    def test_register_and_create(self):
        registry = Registry()
        registry.register("t.double", helpers.Double)
        self.assertIn("t.double", registry)
        self.assertEqual(registry.names(), ("t.double",))
        self.assertIsInstance(registry.create("t.double", {}), helpers.Double)

    def test_one_arg_form_derives_the_name_from_the_class(self):
        registry = Registry()
        registry.register(helpers.Double)
        self.assertIn("helpers.Double", registry)
        self.assertIsInstance(registry.create("helpers.Double", {}), helpers.Double)

    def test_one_arg_form_rejects_a_non_node_class(self):
        registry = Registry()
        with self.assertRaises(TypeError):
            registry.register(int)

    def test_decorator_form(self):
        registry = Registry()

        @registry.node("t.thing")
        class Thing(helpers.Passthrough):
            pass

        self.assertIs(registry.describe("t.thing").node_cls, Thing)

    def test_duplicate_registration_is_rejected(self):
        registry = Registry()
        registry.register("t.double", helpers.Double)
        with self.assertRaises(ValueError):
            registry.register("t.double", helpers.Passthrough)

    def test_registering_a_non_node_is_rejected(self):
        registry = Registry()
        with self.assertRaises(ValueError):
            registry.register("t.nope", object)

    def test_unknown_type_names_the_alternatives(self):
        registry = Registry()
        registry.register("t.double", helpers.Double)
        self.assertIsNone(registry.describe("t.missing"))
        with self.assertRaises(UnknownNodeTypeError) as caught:
            registry.create("t.missing", {})
        self.assertIn("t.double", str(caught.exception))

    def test_describe_does_not_instantiate(self):
        # Validation runs before any node is constructed, so the registry has to
        # answer port and parameter questions from the class.
        class Explodes(Node):
            INPUTS = (PortSpec("input"),)
            OUTPUTS = (PortSpec("output"),)
            PARAMS = {"x": 1}

            def __init__(self, **params):
                raise AssertionError("must not be constructed during validation")

            def run(self, inputs):
                return None

        registry = Registry()
        registry.register("t.explodes", Explodes)
        info = registry.describe("t.explodes")
        self.assertEqual([p.name for p in info.input_ports({})], ["input"])
        self.assertEqual([p.name for p in info.output_ports({})], ["output"])
        self.assertEqual(info.params, {"x": 1})

    def test_factory_registration_carries_explicit_descriptors(self):
        registry = Registry()

        def make(gain: int = 2) -> Node:
            return helpers.EmitN(n=gain)

        registry.register_factory(
            "t.factory",
            make,
            inputs=(PortSpec("input"),),
            outputs=(PortSpec("output"),),
            params={"gain": 2},
        )
        info = registry.describe("t.factory")
        self.assertIsNone(info.node_cls)
        self.assertEqual([p.name for p in info.input_ports({})], ["input"])
        self.assertEqual(info.params, {"gain": 2})
        self.assertEqual(registry.create("t.factory", {"gain": 5}).params["n"], 5)

    def test_merged_registries(self):
        left, right = Registry(), Registry()
        left.register("t.double", helpers.Double)
        right.register("t.passthrough", helpers.Passthrough)
        merged = Registry.merged(left, right)
        self.assertEqual(merged.names(), ("t.double", "t.passthrough"))

    def test_merging_a_conflicting_name_is_rejected(self):
        # A silent last-wins would let an example shadow a core type and quietly
        # change what a serialized blueprint means.
        left, right = Registry(), Registry()
        left.register("t.x", helpers.Double)
        right.register("t.x", helpers.Passthrough)
        with self.assertRaises(ValueError):
            Registry.merged(left, right)

    def test_fixture_registry_is_complete(self):
        registry = helpers.build_registry()
        self.assertIn("helpers.Calib", registry)
        self.assertEqual(len(registry), len(registry.names()))


class TestDocstringExamples(unittest.TestCase):
    def test_doctests_pass(self):
        result = doctest.testmod(
            registry_module,
            globs={"Registry": Registry, "Node": Node, "PortSpec": PortSpec},
            verbose=False,
        )
        self.assertEqual(result.failed, 0)


if __name__ == "__main__":
    unittest.main()
