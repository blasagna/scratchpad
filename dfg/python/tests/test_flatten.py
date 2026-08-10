"""Flattening: qualified IDs, topics, aliases, and boundary resolution.

The topic and alias assertions here are the README's "Naming and namespacing"
section restated as code, so the document cannot drift from the implementation
without a failure here.
"""

import unittest

import helpers
from dfg.blueprint import GraphBuilder, ParamRef
from dfg.errors import ValidationError
from dfg.flatten import find_cycle, flatten, levels, topological_order


class TestReadmeExample(unittest.TestCase):
    def setUp(self):
        self.flat = flatten(helpers.readme_example_spec(), helpers.build_registry())

    def test_qualified_ids_are_dot_joined(self):
        self.assertEqual(
            sorted(self.flat.nodes),
            ["calib", "fusion.predict", "fusion.update", "overlay"],
        )

    def test_a_top_level_node_gets_no_prefix(self):
        self.assertEqual(self.flat.nodes["calib"].qid, "calib")
        self.assertEqual(self.flat.nodes["calib"].scope, ())

    def test_a_nested_node_carries_its_declaring_scope(self):
        node = self.flat.nodes["fusion.predict"]
        self.assertEqual(node.scope, ("fusion",))

    def test_topics_are_exactly_what_the_readme_lists(self):
        self.assertEqual(
            sorted(self.flat.topics),
            [
                "calib.corrected",
                "fusion.predict.state",
                "fusion.update.fused",
                "overlay.composited",
            ],
        )

    def test_the_subgraph_output_is_an_alias_not_a_separate_topic(self):
        # "fusion is itself a node, so its output port pose gives the topic
        # fusion.pose -- which is an alias of fusion.update.fused."
        self.assertEqual(self.flat.aliases["fusion.pose"], ("fusion.update", "fused"))
        self.assertNotIn("fusion.pose", self.flat.topics)

    def test_the_root_output_is_an_alias_of_the_composited_port(self):
        self.assertEqual(self.flat.aliases["pose"], ("overlay", "composited"))
        self.assertEqual(self.flat.outputs["pose"], ("overlay", "composited"))

    def test_an_alias_and_the_aliased_topic_resolve_to_the_same_port(self):
        self.assertEqual(
            self.flat.resolve_topic("fusion.pose"),
            self.flat.resolve_topic("fusion.update.fused"),
        )

    def test_a_subgraph_input_fans_out_into_one_flat_edge_per_target(self):
        # The document draws calib.corrected reaching both predict and update.
        # That is one parent edge into the subgraph's input, which has two targets.
        into_fusion = [
            edge for edge in self.flat.edges if edge.src == ("calib", "corrected")
        ]
        self.assertEqual(
            sorted(edge.dst for edge in into_fusion),
            [("fusion.predict", "imu"), ("fusion.update", "imu")],
        )

    def test_an_edge_out_of_a_subgraph_resolves_through_the_alias(self):
        edge = helpers.present(self.flat.writer_of(("overlay", "pose")))
        self.assertEqual(edge.src, ("fusion.update", "fused"))

    def test_graph_inputs_resolve_to_inner_ports(self):
        self.assertEqual(self.flat.inputs["imu_raw"], (("calib", "raw"),))
        self.assertEqual(self.flat.inputs["frames"], (("overlay", "frame"),))

    def test_ports_come_from_the_registry_in_declaration_order(self):
        update = self.flat.nodes["fusion.update"]
        self.assertEqual([p.name for p in update.inputs], ["imu", "state"])
        self.assertEqual([p.name for p in update.outputs], ["fused"])

    def test_edge_keys_are_readable_and_unique(self):
        keys = [edge.key for edge in self.flat.edges]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertIn("calib.corrected -> fusion.predict.imu", keys)


class TestCapacityAcrossABoundary(unittest.TestCase):
    def test_a_fanning_parent_edge_becomes_independent_queues(self):
        # Flattening's price, asserted so nobody is surprised by it: one declared
        # capacity=64 edge into a two-target subgraph input is two 64-deep queues,
        # not one shared 64-deep queue.
        builder = GraphBuilder("root")
        builder.add("calib", helpers.Calib)
        builder.add_subgraph("fusion", helpers.fusion_subgraph_spec())
        builder.add("overlay", helpers.Overlay)
        builder.connect("calib.corrected", "fusion.imu", capacity=64)
        builder.connect("fusion.pose", "overlay.pose")
        builder.add_input("imu_raw", "calib.raw")
        builder.add_input("frames", "overlay.frame")
        builder.add_output("pose", "overlay.composited")

        flat = flatten(builder.build(), helpers.build_registry())
        fanned = [e for e in flat.edges if e.src == ("calib", "corrected")]
        self.assertEqual(len(fanned), 2)
        self.assertEqual([e.capacity for e in fanned], [64, 64])


class TestNestingTwoDeep(unittest.TestCase):
    def build(self):
        inner = GraphBuilder("inner")
        inner.add("double", helpers.Double)
        inner.add_input("x", "double.input")
        inner.add_output("y", "double.output")
        inner_spec = inner.build()

        middle = GraphBuilder("middle")
        middle.add_subgraph("deep", inner_spec)
        middle.add_input("x", "deep.x")
        middle.add_output("y", "deep.y")
        middle_spec = middle.build()

        root = GraphBuilder("root")
        root.add_subgraph("mid", middle_spec)
        root.add("tail", helpers.Passthrough)
        root.connect("mid.y", "tail.input")
        root.add_input("source", "mid.x")
        root.add_output("result", "tail.output")
        return root.build()

    def test_alias_chains_resolve_through_every_level(self):
        flat = flatten(self.build(), helpers.build_registry())
        self.assertIn("mid.deep.double", flat.nodes)
        self.assertEqual(flat.aliases["mid.deep.y"], ("mid.deep.double", "output"))
        self.assertEqual(flat.aliases["mid.y"], ("mid.deep.double", "output"))
        self.assertEqual(flat.outputs["result"], ("tail", "output"))
        self.assertEqual(flat.inputs["source"], (("mid.deep.double", "input"),))

    def test_an_edge_across_two_boundaries_lands_on_the_real_port(self):
        flat = flatten(self.build(), helpers.build_registry())
        self.assertEqual(
            helpers.present(flat.writer_of(("tail", "input"))).src,
            ("mid.deep.double", "output"),
        )


class TestSiblingNamespaces(unittest.TestCase):
    def test_two_subgraphs_may_both_declare_an_output_named_pose(self):
        # "An alias is resolved within the graph that declares it, so names only
        # have to be unique among their siblings."
        root = GraphBuilder("root")
        root.add_subgraph("left", helpers.fusion_subgraph_spec())
        root.add_subgraph("right", helpers.fusion_subgraph_spec())
        root.add("merge", helpers.Sum2)
        root.connect("left.pose", "merge.a")
        root.connect("right.pose", "merge.b")
        root.add_input("left_imu", "left.imu")
        root.add_input("right_imu", "right.imu")
        root.add_output("total", "merge.output")

        flat = flatten(root.build(), helpers.build_registry())
        self.assertEqual(flat.aliases["left.pose"], ("left.update", "fused"))
        self.assertEqual(flat.aliases["right.pose"], ("right.update", "fused"))
        self.assertIn("left.predict.state", flat.topics)
        self.assertIn("right.predict.state", flat.topics)


class TestGraphParameters(unittest.TestCase):
    def build(self, *, override=None):
        inner = GraphBuilder("inner", params={"n": 2})
        inner.add("emit", helpers.EmitN, params={"n": ParamRef("n")})
        inner.add_input("x", "emit.input")
        inner.add_output("y", "emit.output")

        root = GraphBuilder("root", params={"outer_n": 5})
        root.add_subgraph("sub", inner.build(), params=override or {})
        root.add_input("source", "sub.x")
        root.add_output("result", "sub.y")
        return root.build()

    def test_a_param_ref_takes_the_subgraphs_own_default(self):
        flat = flatten(self.build(), helpers.build_registry())
        self.assertEqual(flat.nodes["sub.emit"].params, {"n": 2})

    def test_a_parent_override_wins(self):
        flat = flatten(self.build(override={"n": 9}), helpers.build_registry())
        self.assertEqual(flat.nodes["sub.emit"].params, {"n": 9})

    def test_an_override_may_itself_reference_the_parents_params(self):
        # This is what makes a subgraph reusable: the same blueprint at 200 Hz in
        # one parent and 100 Hz in another.
        flat = flatten(
            self.build(override={"n": ParamRef("outer_n")}), helpers.build_registry()
        )
        self.assertEqual(flat.nodes["sub.emit"].params, {"n": 5})

    def test_an_unresolvable_reference_is_reported(self):
        inner = GraphBuilder("inner")
        inner.add("emit", helpers.EmitN, params={"n": ParamRef("missing")})
        inner.add_input("x", "emit.input")
        inner.add_output("y", "emit.output")
        root = GraphBuilder("root")
        root.add_subgraph("sub", inner.build())
        root.add_input("source", "sub.x")
        root.add_output("result", "sub.y")
        with self.assertRaises(ValidationError) as caught:
            flatten(root.build(), helpers.build_registry())
        self.assertEqual(
            [p.code for p in caught.exception.problems], ["unresolved_param"]
        )


class TestGraphAlgorithms(unittest.TestCase):
    def test_topological_order_is_canonical(self):
        flat = flatten(helpers.readme_example_spec(), helpers.build_registry())
        order = topological_order(flat)
        self.assertEqual(order, ("calib", "fusion.predict", "fusion.update", "overlay"))
        self.assertEqual(order, topological_order(flat))

    def test_ties_in_the_topological_order_break_by_qualified_id(self):
        # Two structurally symmetric nodes, declared b-then-a. The order must be
        # a-then-b regardless, or replays can legitimately differ.
        builder = GraphBuilder("g")
        builder.add("n_b", helpers.Double)
        builder.add("n_a", helpers.Double)
        builder.add("merge", helpers.Sum2)
        builder.connect("n_b.output", "merge.b")
        builder.connect("n_a.output", "merge.a")
        builder.add_input("left", "n_b.input")
        builder.add_input("right", "n_a.input")
        builder.add_output("output", "merge.output")
        flat = flatten(builder.build(), helpers.build_registry())
        self.assertEqual(topological_order(flat), ("n_a", "n_b", "merge"))

    def test_levels_use_the_longest_path(self):
        # tail is fed by both head (level 0) and mid (level 1), so it is level 2.
        # Shortest-path levels would put it at 1, alongside its own predecessor.
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double)
        builder.add("mid", helpers.Double)
        builder.add("tail", helpers.Sum2)
        builder.connect("head.output", "mid.input")
        builder.connect("head.output", "tail.a")
        builder.connect("mid.output", "tail.b")
        builder.add_input("source", "head.input")
        builder.add_output("output", "tail.output")
        flat = flatten(builder.build(), helpers.build_registry())
        self.assertEqual(levels(flat), {"head": 0, "mid": 1, "tail": 2})

    def test_no_cycle_in_a_valid_graph(self):
        flat = flatten(helpers.readme_example_spec(), helpers.build_registry())
        self.assertIsNone(find_cycle(flat))

    def test_a_cycle_is_found_and_named(self):
        builder = GraphBuilder("g")
        builder.add("a", helpers.Sum2)
        builder.add("b", helpers.Double)
        builder.connect("a.output", "b.input")
        builder.connect("b.output", "a.b")
        builder.add_input("source", "a.a")
        builder.add_output("output", "b.output")
        flat = flatten(builder.build(), helpers.build_registry())
        cycle = find_cycle(flat)
        self.assertIsNotNone(cycle)
        self.assertEqual(set(cycle), {"a", "b"})
        with self.assertRaises(ValidationError):
            topological_order(flat)


if __name__ == "__main__":
    unittest.main()
