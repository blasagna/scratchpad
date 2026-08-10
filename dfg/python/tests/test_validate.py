"""Blueprint validation: one test per rejection, plus the aggregation promise."""

import unittest
from typing import Annotated, NamedTuple

import helpers
from dfg.blueprint import (
    EdgeSpec,
    GraphBuilder,
    GraphInput,
    GraphSpec,
    NodeSpec,
    ParamRef,
    PortRef,
    SubgraphSpec,
)
from dfg.errors import ValidationError
from dfg.node import REQUIRED, Emit, In, Node
from dfg.ports import Port, PortSpec
from dfg.readiness import AnyInput, CountAtLeast, PredicateRule, ReadinessRule
from dfg.registry import Registry
from dfg.validate import check, validate


def codes(spec, registry=None):
    """The problem codes a blueprint produces, sorted."""
    return sorted(p.code for p in check(spec, registry or helpers.build_registry()))


def valid_spec():
    """A minimal blueprint with nothing wrong with it."""
    builder = GraphBuilder("g")
    builder.add("double", helpers.Double)
    builder.add_input("source", "double.input")
    builder.add_output("result", "double.output")
    return builder.build()


class TestAcceptance(unittest.TestCase):
    def test_a_valid_graph_has_no_problems(self):
        self.assertEqual(check(valid_spec(), helpers.build_registry()), ())
        validate(valid_spec(), helpers.build_registry())

    def test_the_readme_example_is_valid(self):
        validate(helpers.readme_example_spec(), helpers.build_registry())

    def test_validate_raises_with_every_problem_attached(self):
        spec = GraphSpec(
            name="g",
            nodes=(
                NodeSpec(node_id="a", type_name="t.nope"),
                NodeSpec(node_id="a", type_name="t.double"),
            ),
        )
        with self.assertRaises(ValidationError) as caught:
            validate(spec, helpers.build_registry())
        self.assertGreaterEqual(len(caught.exception.problems), 2)
        self.assertIn("problem(s)", str(caught.exception))


class TestAggregation(unittest.TestCase):
    def test_several_problems_are_reported_together(self):
        # Fixing problems one run at a time is a poor way to spend an afternoon.
        spec = GraphSpec(
            name="g",
            nodes=(
                NodeSpec(node_id="dup", type_name="helpers.Double"),
                NodeSpec(node_id="dup", type_name="helpers.Double"),
                NodeSpec(node_id="bad.id", type_name="helpers.Double"),
                NodeSpec(node_id="unknown", type_name="t.nope"),
            ),
            edges=(EdgeSpec(PortRef("dup", "output"), PortRef("missing", "input")),),
            inputs=(
                GraphInput("a", (PortRef("dup", "input"),)),
                GraphInput("b", (PortRef("bad.id", "input"),)),
            ),
        )
        self.assertEqual(
            codes(spec),
            ["bad_name", "dangling_edge", "duplicate_id", "unknown_type"],
        )


class TestIdentityProblems(unittest.TestCase):
    def test_duplicate_sibling_node_ids(self):
        builder = GraphBuilder("g")
        builder.add("twin", helpers.Double)
        builder.add("twin", helpers.Passthrough)
        builder.add_input("source", "twin.input")
        self.assertIn("duplicate_id", codes(builder.build()))

    def test_duplicate_boundary_names(self):
        builder = GraphBuilder("g")
        builder.add("double", helpers.Double)
        builder.add_output("result", "double.output")
        builder.add_output("result", "double.output")
        builder.add_input("source", "double.input")
        self.assertIn("duplicate_id", codes(builder.build()))

    def test_a_dotted_node_id_is_rejected(self):
        # Qualified IDs and topics are dotted paths, so a node ID may not be.
        spec = GraphSpec(
            name="g", nodes=(NodeSpec(node_id="a.b", type_name="t.double"),)
        )
        problems = check(spec, helpers.build_registry())
        self.assertIn("bad_name", [p.code for p in problems])
        self.assertIn("contains a dot", " ".join(p.detail for p in problems))

    def test_other_illegal_node_ids_are_rejected(self):
        for node_id in ("1a", "has space", "_leading"):
            with self.subTest(node_id=node_id):
                spec = GraphSpec(
                    name="g", nodes=(NodeSpec(node_id=node_id, type_name="t.double"),)
                )
                self.assertIn("bad_name", codes(spec))

    def test_a_reserved_port_name_is_rejected(self):
        class Reserved(Node):
            INPUTS = (PortSpec("__secret__"),)
            OUTPUTS = (PortSpec("output"),)

            def run(self, inputs):
                return None

        registry = helpers.build_registry()
        registry.register("t.reserved", Reserved)
        spec = GraphSpec(
            name="g", nodes=(NodeSpec(node_id="a", type_name="t.reserved"),)
        )
        self.assertIn("reserved_name", codes(spec, registry))


class TestTypeAndParamProblems(unittest.TestCase):
    def test_unknown_node_type_names_the_alternatives(self):
        spec = GraphSpec(name="g", nodes=(NodeSpec(node_id="a", type_name="t.nope"),))
        problems = check(spec, helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["unknown_type"])
        self.assertIn("helpers.Double", problems[0].detail)

    def test_unknown_parameter(self):
        builder = GraphBuilder("g")
        builder.add("emit", helpers.EmitN, params={"nope": 1})
        builder.add_input("source", "emit.input")
        self.assertIn("unknown_param", codes(builder.build()))

    def test_missing_required_parameter(self):
        class NeedsSize(Node):
            INPUTS = (PortSpec("input"),)
            OUTPUTS = (PortSpec("output"),)
            PARAMS = {"size": REQUIRED}

            def run(self, inputs):
                return None

        registry = helpers.build_registry()
        registry.register(NeedsSize)
        builder = GraphBuilder("g")
        builder.add("n", NeedsSize)
        builder.add_input("source", "n.input")
        self.assertIn("missing_param", codes(builder.build(), registry))

    def test_an_unresolvable_param_ref(self):
        builder = GraphBuilder("g")
        builder.add("emit", helpers.EmitN, params={"n": ParamRef("missing")})
        builder.add_input("source", "emit.input")
        self.assertIn("unresolved_param", codes(builder.build()))

    def test_a_resolvable_param_ref_is_accepted(self):
        builder = GraphBuilder("g", params={"n": 4})
        builder.add("emit", helpers.EmitN, params={"n": ParamRef("n")})
        builder.add_input("source", "emit.input")
        builder.add_output("output", "emit.output")
        self.assertEqual(codes(builder.build()), [])

    def test_a_subgraph_override_of_an_undeclared_param(self):
        root = GraphBuilder("root")
        root.add_subgraph("sub", helpers.fusion_subgraph_spec(), params={"nope": 1})
        root.add_input("source", "sub.imu")
        root.add_output("output", "sub.pose")
        self.assertIn("unknown_param", codes(root.build()))


class TestWiringProblems(unittest.TestCase):
    def test_an_edge_naming_a_missing_node(self):
        builder = GraphBuilder("g")
        builder.add("double", helpers.Double)
        builder.connect("double.output", "ghost.input")
        builder.add_input("source", "double.input")
        self.assertIn("dangling_edge", codes(builder.build()))

    def test_an_edge_naming_a_missing_port(self):
        builder = GraphBuilder("g")
        builder.add("double", helpers.Double)
        builder.add("sink", helpers.Passthrough)
        builder.connect("double.nope", "sink.input")
        builder.add_input("source", "double.input")
        self.assertIn("dangling_edge", codes(builder.build()))

    def test_an_edge_naming_a_missing_subgraph_boundary(self):
        root = GraphBuilder("root")
        root.add_subgraph("fusion", helpers.fusion_subgraph_spec())
        root.add("sink", helpers.Passthrough)
        root.connect("fusion.nope", "sink.input")
        root.add_input("source", "fusion.imu")
        self.assertIn("dangling_edge", codes(root.build()))

    def test_an_output_aliasing_a_missing_port(self):
        builder = GraphBuilder("g")
        builder.add("double", helpers.Double)
        builder.add_input("source", "double.input")
        builder.add_output("result", "double.nope")
        self.assertIn("dangling_edge", codes(builder.build()))

    def test_an_input_with_no_targets(self):
        spec = GraphSpec(
            name="g",
            nodes=(NodeSpec(node_id="double", type_name="t.double"),),
            inputs=(GraphInput(name="source", targets=()),),
        )
        self.assertIn("dangling_boundary", codes(spec))

    def test_two_writers_into_one_input_port(self):
        # An input port takes one writer: two producers into one queue would make
        # message order depend on which the scheduler fired first.
        builder = GraphBuilder("g")
        builder.add("left", helpers.Double)
        builder.add("right", helpers.Double)
        builder.add("sink", helpers.Passthrough)
        builder.connect("left.output", "sink.input")
        builder.connect("right.output", "sink.input")
        builder.add_input("a", "left.input")
        builder.add_input("b", "right.input")
        builder.add_output("output", "sink.output")
        problems = check(builder.build(), helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["fan_in"])
        self.assertIn("merge node", problems[0].detail)

    def test_an_edge_and_a_graph_input_into_the_same_port(self):
        builder = GraphBuilder("g")
        builder.add("left", helpers.Double)
        builder.add("sink", helpers.Passthrough)
        builder.connect("left.output", "sink.input")
        builder.add_input("a", "left.input")
        builder.add_input("b", "sink.input")
        builder.add_output("output", "sink.output")
        self.assertIn("fan_in", codes(builder.build()))

    def test_a_boundary_target_and_an_inner_edge_into_one_port(self):
        # A parent edge into fusion.imu resolves to this subgraph's own boundary
        # target, so the writer that arrives "from outside" is counted in the same
        # scope as the inner edge competing with it. That is why the per-scope
        # check is complete.
        inner = GraphBuilder("fusion")
        inner.add("predict", helpers.Predict)
        inner.add("update", helpers.Update)
        inner.add("extra", helpers.Calib)
        inner.connect("predict.state", "update.state")
        inner.connect("extra.corrected", "predict.imu")
        inner.add_input("imu", "predict.imu")
        inner.add_input("raw", "extra.raw")
        inner.add_input("also", "update.imu")
        inner.add_output("pose", "update.fused")
        problems = check(inner.build(), helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["fan_in"])
        self.assertIn("predict.imu", problems[0].detail)

    def test_fan_out_from_one_output_is_fine(self):
        # Fan-out is explicitly allowed and is still one topic.
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double)
        builder.add("left", helpers.Passthrough)
        builder.add("right", helpers.Passthrough)
        builder.connect("head.output", "left.input")
        builder.connect("head.output", "right.input")
        builder.add_input("source", "head.input")
        builder.add_output("a", "left.output")
        builder.add_output("b", "right.output")
        self.assertEqual(codes(builder.build()), [])


class TestFiringProblems(unittest.TestCase):
    def test_a_node_with_no_input_ports_is_rejected(self):
        # Graph inputs are the only sources.
        builder = GraphBuilder("g")
        builder.add("src", helpers.NoInputs)
        builder.add("sink", helpers.Passthrough)
        builder.connect("src.output", "sink.input")
        builder.add_output("output", "sink.output")
        problems = check(builder.build(), helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["source_node"])
        self.assertIn("graph inputs are the only sources", problems[0].detail)

    def test_all_readiness_with_an_unwired_port_can_never_fire(self):
        builder = GraphBuilder("g")
        builder.add("sum", helpers.Sum2)
        builder.add_input("a", "sum.a")
        builder.add_output("output", "sum.output")
        problems = check(builder.build(), helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["never_ready"])
        self.assertIn("'b'", problems[0].detail)

    def test_any_readiness_with_an_unwired_port_is_allowed(self):
        builder = GraphBuilder("g")
        builder.add("sum", helpers.Sum2, readiness=AnyInput())
        builder.add_input("a", "sum.a")
        builder.add_output("output", "sum.output")
        self.assertEqual(codes(builder.build()), [])

    def test_an_unregistered_readiness_kind_cannot_round_trip(self):
        class Homemade(ReadinessRule):
            KIND = "homemade"

            def is_ready(self, queues):
                return True

        builder = GraphBuilder("g")
        builder.add("double", helpers.Double, readiness=Homemade())
        builder.add_input("source", "double.input")
        builder.add_output("output", "double.output")
        self.assertIn("unknown_readiness", codes(builder.build()))

    def test_a_predicate_rule_is_legal_in_memory(self):
        # It just cannot be serialized, which serialize.dumps says, not validation.
        builder = GraphBuilder("g")
        builder.add(
            "double", helpers.Double, readiness=PredicateRule(lambda queues: True)
        )
        builder.add_input("source", "double.input")
        builder.add_output("output", "double.output")
        self.assertEqual(codes(builder.build()), [])

    def test_a_registered_rule_with_arguments_is_accepted(self):
        builder = GraphBuilder("g")
        builder.add("double", helpers.Double, readiness=CountAtLeast(4))
        builder.add_input("source", "double.input")
        builder.add_output("output", "double.output")
        self.assertEqual(codes(builder.build()), [])

    def test_a_non_rule_readiness_is_rejected(self):
        spec = GraphSpec(
            name="g",
            nodes=(
                # A readiness rule is an object, not a name; validate reports the
                # bad value rather than the constructor refusing it.
                NodeSpec(
                    node_id="double",
                    type_name="helpers.Double",
                    readiness="all",  # pyrefly: ignore[bad-argument-type]
                ),
            ),
            inputs=(GraphInput("source", (PortRef("double", "input"),)),),
        )
        self.assertIn("bad_readiness", codes(spec))


class TestPolicyProblems(unittest.TestCase):
    def test_an_unknown_error_policy(self):
        builder = GraphBuilder("g")
        builder.add("double", helpers.Double, on_error="explode")
        builder.add_input("source", "double.input")
        builder.add_output("output", "double.output")
        self.assertIn("bad_policy", codes(builder.build()))

    def test_block_is_not_an_overflow_policy(self):
        # It deadlocks a single-threaded scheduler instantly.
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double)
        builder.add("sink", helpers.Passthrough)
        builder.connect("head.output", "sink.input", capacity=2, on_overflow="block")
        builder.add_input("source", "head.input")
        builder.add_output("output", "sink.output")
        problems = check(builder.build(), helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["bad_policy"])
        self.assertIn("'block'", problems[0].detail)


class Texty(Node):
    class Out(NamedTuple):
        output: Annotated[Emit[str], Port("text")]

    def run(self, *, input: Annotated[In[str], Port("text")] = ()) -> Out | None:
        return None


class TestTypeTags(unittest.TestCase):
    def registry_with_tagged_node(self):
        registry = helpers.build_registry()
        registry.register(Texty)
        return registry

    def test_mismatched_tags_are_rejected(self):
        builder = GraphBuilder("g")
        builder.add("num", helpers.Double)  # out is tagged "number"
        builder.add("text", Texty)  # in is tagged "text"
        builder.connect("num.output", "text.input")
        builder.add_input("source", "num.input")
        builder.add_output("output", "text.output")
        problems = check(builder.build(), self.registry_with_tagged_node())
        self.assertEqual([p.code for p in problems], ["type_mismatch"])

    def test_matching_tags_are_accepted(self):
        builder = GraphBuilder("g")
        builder.add("a", helpers.Double)
        builder.add("b", helpers.Double)
        builder.connect("a.output", "b.input")
        builder.add_input("source", "a.input")
        builder.add_output("output", "b.output")
        self.assertEqual(codes(builder.build()), [])

    def test_an_untyped_end_is_never_a_mismatch(self):
        # Untyped is the default, so this check is allowed to stay silent.
        builder = GraphBuilder("g")
        builder.add("num", helpers.Double)  # tagged
        builder.add("any", helpers.Passthrough)  # untagged
        builder.connect("num.output", "any.input")
        builder.add_input("source", "num.input")
        builder.add_output("output", "any.output")
        self.assertEqual(codes(builder.build()), [])

    def test_a_boundary_tag_is_checked_against_its_target(self):
        builder = GraphBuilder("g")
        builder.add("num", helpers.Double)
        builder.add_input("source", "num.input", type_tag="text")
        builder.add_output("output", "num.output")
        self.assertIn("type_mismatch", codes(builder.build()))

    def test_a_boundary_tag_is_checked_against_its_alias(self):
        builder = GraphBuilder("g")
        builder.add("num", helpers.Double)
        builder.add_input("source", "num.input")
        builder.add_output("output", "num.output", type_tag="text")
        self.assertIn("type_mismatch", codes(builder.build()))


class TestCycles(unittest.TestCase):
    def test_a_direct_cycle_is_rejected(self):
        builder = GraphBuilder("g")
        builder.add("a", helpers.Sum2)
        builder.add("b", helpers.Double)
        builder.connect("a.output", "b.input")
        builder.connect("b.output", "a.b")
        builder.add_input("source", "a.a")
        builder.add_output("output", "b.output")
        problems = check(builder.build(), helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["cycle"])
        self.assertIn("->", problems[0].detail)

    def test_a_cycle_through_a_subgraph_is_rejected(self):
        # Checked on the flattened graph, which is the only exact place: treating
        # the subgraph as one vertex would also flag graphs that are really fine.
        inner = GraphBuilder("inner")
        inner.add("mid", helpers.Double)
        inner.add_input("x", "mid.input")
        inner.add_output("y", "mid.output")

        root = GraphBuilder("root")
        root.add_subgraph("sub", inner.build())
        root.add("outer", helpers.Sum2)
        root.connect("sub.y", "outer.a")
        root.connect("outer.output", "sub.x")
        root.add_input("source", "outer.b")
        root.add_output("output", "sub.y")
        self.assertEqual(codes(root.build()), ["cycle"])

    def test_a_diamond_is_not_a_cycle(self):
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double)
        builder.add("left", helpers.Passthrough)
        builder.add("right", helpers.Passthrough)
        builder.add("join", helpers.Sum2)
        builder.connect("head.output", "left.input")
        builder.connect("head.output", "right.input")
        builder.connect("left.output", "join.a")
        builder.connect("right.output", "join.b")
        builder.add_input("source", "head.input")
        builder.add_output("output", "join.output")
        self.assertEqual(codes(builder.build()), [])


class TestNestedScopes(unittest.TestCase):
    def test_a_problem_inside_a_subgraph_names_its_scope(self):
        inner = GraphSpec(
            name="inner", nodes=(NodeSpec(node_id="bad", type_name="t.nope"),)
        )
        root = GraphSpec(name="root", nodes=(SubgraphSpec(node_id="sub", graph=inner),))
        problems = check(root, helpers.build_registry())
        self.assertEqual([p.code for p in problems], ["unknown_type"])
        self.assertEqual(problems[0].scope, ("sub",))
        self.assertIn("in sub:", str(problems[0]))

    def test_a_root_problem_says_root(self):
        spec = GraphSpec(name="g", nodes=(NodeSpec(node_id="a", type_name="t.nope"),))
        self.assertIn("<root>", str(check(spec, helpers.build_registry())[0]))

    def test_an_empty_registry_reports_every_type(self):
        problems = check(helpers.readme_example_spec(), Registry())
        self.assertEqual({p.code for p in problems}, {"unknown_type"})
        self.assertEqual(len(problems), 4)


if __name__ == "__main__":
    unittest.main()
