"""Mermaid rendering from a blueprint, pinned to a golden string."""

import unittest

import helpers
from dfg.blueprint import GraphBuilder
from dfg.mermaid import render_mermaid
from dfg.readiness import AnyInput

README_EXAMPLE = """\
flowchart LR
  imu_raw__in([imu_raw])
  imu_raw__in --> calib
  frames__in([frames])
  frames__in --> overlay
  pose__out([pose])
  overlay -- "overlay.composited" --> pose__out
  calib[calib]
  subgraph fusion[fusion]
    fusion__predict[predict]
    fusion__update[update]
    fusion__predict -- "fusion.predict.state" --> fusion__update
  end
  overlay[overlay]
  calib -- "calib.corrected" --> fusion
  fusion -- "fusion.pose" --> overlay"""


class TestReadmeExample(unittest.TestCase):
    def test_matches_the_golden(self):
        self.assertEqual(render_mermaid(helpers.readme_example_spec()), README_EXAMPLE)

    def test_is_stable_across_calls(self):
        spec = helpers.readme_example_spec()
        self.assertEqual(render_mermaid(spec), render_mermaid(spec))

    def test_draws_the_subgraph_as_a_block(self):
        rendered = render_mermaid(helpers.readme_example_spec())
        self.assertIn("subgraph fusion[fusion]", rendered)
        self.assertIn("\n  end", rendered)

    def test_draws_boundaries_as_stadium_nodes(self):
        rendered = render_mermaid(helpers.readme_example_spec())
        self.assertIn("([imu_raw])", rendered)
        self.assertIn("([pose])", rendered)

    def test_labels_the_inner_edge_with_its_namespaced_topic(self):
        # Which is the whole reason to render topics: the label shows that the
        # subgraph ID namespaced the node.
        self.assertIn(
            '"fusion.predict.state"', render_mermaid(helpers.readme_example_spec())
        )


class TestOptions(unittest.TestCase):
    def test_direction_is_honoured(self):
        self.assertTrue(
            render_mermaid(helpers.readme_example_spec(), direction="TB").startswith(
                "flowchart TB"
            )
        )

    def test_an_unknown_direction_is_rejected(self):
        with self.assertRaises(ValueError):
            render_mermaid(helpers.readme_example_spec(), direction="sideways")

    def test_topics_can_be_turned_off(self):
        rendered = render_mermaid(helpers.readme_example_spec(), show_topics=False)
        self.assertNotIn('"', rendered)
        self.assertIn("calib --> fusion", rendered)


class TestPolicyMarkers(unittest.TestCase):
    def test_non_default_policies_are_drawn_on_the_node(self):
        builder = GraphBuilder("g")
        builder.add("plain", helpers.Double)
        builder.add(
            "marked", helpers.Passthrough, readiness=AnyInput(), on_error="drop"
        )
        builder.connect("plain.out", "marked.in")
        builder.add_input("source", "plain.in")
        builder.add_output("out", "marked.out")
        rendered = render_mermaid(builder.build())
        self.assertIn("plain[plain]", rendered)
        self.assertIn("marked[marked<br/>any/drop]", rendered)


class TestUnvalidatedBlueprints(unittest.TestCase):
    def test_a_blueprint_that_does_not_validate_still_renders(self):
        # Renderable before runnable: a diagram is something you can get from a
        # description that is still wrong.
        builder = GraphBuilder("broken")
        builder.add("a", "type.that.is.not.registered")
        builder.connect("a.out", "ghost.in")
        rendered = render_mermaid(builder.build())
        self.assertIn("a[a]", rendered)
        self.assertIn('a -- "a.out" --> ghost', rendered)


if __name__ == "__main__":
    unittest.main()
