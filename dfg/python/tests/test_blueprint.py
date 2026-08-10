"""Blueprint specs and the builder that produces them."""

import doctest
import unittest

import helpers
from dfg import blueprint
from dfg.blueprint import (
    EdgeSpec,
    GraphBuilder,
    GraphInput,
    GraphOutput,
    GraphSpec,
    NodeSpec,
    PortRef,
    SubgraphSpec,
    split_endpoint,
)
from dfg.node import Node
from dfg.ports import PortSpec
from dfg.readiness import AllInputs, AnyInput


class TestEndpointParsing(unittest.TestCase):
    def test_splits_at_the_last_dot(self):
        self.assertEqual(
            split_endpoint("calib.corrected"), PortRef("calib", "corrected")
        )

    def test_rejects_an_endpoint_with_no_dot(self):
        with self.assertRaises(ValueError) as caught:
            split_endpoint("calib")
        self.assertIn("node.port", str(caught.exception))


class TestSpecs(unittest.TestCase):
    def test_specs_are_frozen(self):
        spec = NodeSpec(node_id="a", type_name="t.double")
        with self.assertRaises(Exception):
            # The assignment being a type error is the point of the test.
            spec.node_id = "b"  # pyrefly: ignore[read-only]

    def test_node_spec_defaults(self):
        spec = NodeSpec(node_id="a", type_name="t.double")
        self.assertEqual(spec.params, {})
        self.assertEqual(spec.readiness, AllInputs())
        self.assertEqual(spec.on_error, "stop")
        self.assertEqual(spec.priority, 0)

    def test_node_spec_copies_the_params_mapping(self):
        params = {"n": 1}
        spec = NodeSpec(node_id="a", type_name="t.emit_n", params=params)
        params["n"] = 99
        self.assertEqual(spec.params, {"n": 1})

    def test_edge_defaults_to_unbounded(self):
        edge = EdgeSpec(PortRef("a", "output"), PortRef("b", "inp"))
        self.assertIsNone(edge.capacity)
        self.assertEqual(edge.on_overflow, "error")
        self.assertEqual(edge.transport, "memory")

    def test_edge_rejects_a_capacity_below_one(self):
        for capacity in (0, -1):
            with self.subTest(capacity=capacity), self.assertRaises(ValueError):
                EdgeSpec(PortRef("a", "output"), PortRef("b", "inp"), capacity=capacity)

    def test_specs_compare_by_value(self):
        # Round-trip tests rely on this: a reloaded blueprint must equal the
        # original, not merely resemble it.
        self.assertEqual(
            NodeSpec(node_id="a", type_name="t.double", params={"x": 1}),
            NodeSpec(node_id="a", type_name="t.double", params={"x": 1}),
        )
        self.assertNotEqual(
            NodeSpec(node_id="a", type_name="t.double", readiness=AllInputs()),
            NodeSpec(node_id="a", type_name="t.double", readiness=AnyInput()),
        )
        self.assertEqual(helpers.readme_example_spec(), helpers.readme_example_spec())

    def test_lookup_helpers(self):
        spec = helpers.readme_example_spec()
        self.assertIsInstance(spec.node("calib"), NodeSpec)
        self.assertIsInstance(spec.node("fusion"), SubgraphSpec)
        self.assertIsNone(spec.node("nope"))
        self.assertEqual(
            helpers.present(spec.input("imu_raw")).targets,
            (PortRef("calib", "raw"),),
        )
        self.assertEqual(
            helpers.present(spec.output("pose")).source,
            PortRef("overlay", "composited"),
        )
        self.assertIsNone(spec.output("nope"))


class TestGraphBuilder(unittest.TestCase):
    def test_add_accepts_a_node_class(self):
        builder = GraphBuilder("g")
        builder.add("double", helpers.Double)
        self.assertEqual(helpers.node_spec(builder.build()).type_name, "helpers.Double")

    def test_add_rejects_a_non_node_class(self):
        builder = GraphBuilder("g")
        with self.assertRaises(TypeError):
            # Passing a non-Node class is what is under test.
            builder.add("n", int)  # pyrefly: ignore[bad-argument-type]

    def test_builds_the_spec_a_hand_written_literal_would(self):
        builder = GraphBuilder("chain", params={"gain": 2})
        builder.add("double", "t.double")
        builder.add("tail", "t.passthrough", on_error="drop", priority=5)
        builder.connect(
            "double.output", "tail.inp", capacity=4, on_overflow="drop_oldest"
        )
        builder.add_input("samples", "double.inp", type_tag="number")
        builder.add_output("result", "tail.output")

        self.assertEqual(
            builder.build(),
            GraphSpec(
                name="chain",
                nodes=(
                    NodeSpec(node_id="double", type_name="t.double"),
                    NodeSpec(
                        node_id="tail",
                        type_name="t.passthrough",
                        on_error="drop",
                        priority=5,
                    ),
                ),
                edges=(
                    EdgeSpec(
                        src=PortRef("double", "output"),
                        dst=PortRef("tail", "inp"),
                        capacity=4,
                        on_overflow="drop_oldest",
                    ),
                ),
                inputs=(
                    GraphInput(
                        name="samples",
                        targets=(PortRef("double", "inp"),),
                        type_tag="number",
                    ),
                ),
                outputs=(GraphOutput(name="result", source=PortRef("tail", "output")),),
                params={"gain": 2},
            ),
        )

    def test_add_returns_the_node_id(self):
        builder = GraphBuilder("g")
        self.assertEqual(builder.add("double", "t.double"), "double")
        self.assertEqual(
            builder.add_subgraph("fusion", helpers.fusion_subgraph_spec()), "fusion"
        )

    def test_an_input_may_fan_out_to_several_targets(self):
        builder = GraphBuilder("g")
        builder.add("left", "t.double")
        builder.add("right", "t.double")
        builder.add_input("shared", "left.inp", "right.inp")
        (boundary,) = builder.build().inputs
        self.assertEqual(
            boundary.targets, (PortRef("left", "inp"), PortRef("right", "inp"))
        )

    def test_build_is_repeatable_and_independent(self):
        builder = GraphBuilder("g")
        builder.add("double", "t.double")
        first = builder.build()
        builder.add("second", "t.double")
        self.assertEqual(len(first.nodes), 1)
        self.assertEqual(len(builder.build().nodes), 2)

    def test_readme_example_has_the_shape_the_document_draws(self):
        spec = helpers.readme_example_spec()
        self.assertEqual(
            [node.node_id for node in spec.nodes], ["calib", "fusion", "overlay"]
        )
        self.assertEqual([b.name for b in spec.inputs], ["imu_raw", "frames"])
        self.assertEqual([b.name for b in spec.outputs], ["pose"])
        # One parent edge into the subgraph's input, which fans out inside it --
        # that is how the document's two calib.corrected arrows are spelled.
        self.assertEqual(
            [(str(e.src), str(e.dst)) for e in spec.edges],
            [("calib.corrected", "fusion.imu"), ("fusion.pose", "overlay.pose")],
        )
        fusion = helpers.subgraph_spec(spec, "fusion")
        self.assertEqual(len(helpers.present(fusion.graph.input("imu")).targets), 2)


class TestDocstringExamples(unittest.TestCase):
    def test_doctests_pass(self):
        registry_globals = {
            "GraphBuilder": GraphBuilder,
            "split_endpoint": split_endpoint,
            "PortRef": PortRef,
            "Node": Node,
            "PortSpec": PortSpec,
        }
        result = doctest.testmod(blueprint, globs=registry_globals, verbose=False)
        self.assertEqual(result.failed, 0)


if __name__ == "__main__":
    unittest.main()
