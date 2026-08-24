"""Mermaid rendering from a blueprint, pinned to a golden string."""

import unittest

import helpers
from dfg.blueprint import GraphBuilder
from dfg.mermaid import render_mermaid
from dfg.readiness import AnyInput

README_EXAMPLE = """\
flowchart LR
  readings__in([readings])
  readings__in --> scale
  tags__in([tags])
  tags__in --> relabel
  result__out([result])
  relabel -- "relabel.labeled" --> result__out
  scale[scale]
  subgraph classify[classify]
    classify__flag[flag]
    classify__grade[grade]
    classify__flag -- "classify.flag.flagged" --> classify__grade
  end
  relabel[relabel]
  scale -- "scale.scaled" --> classify
  classify -- "classify.classified" --> relabel"""


class TestReadmeExample(unittest.TestCase):
    def test_matches_the_golden(self):
        self.assertEqual(render_mermaid(helpers.readme_example_spec()), README_EXAMPLE)

    def test_is_stable_across_calls(self):
        spec = helpers.readme_example_spec()
        self.assertEqual(render_mermaid(spec), render_mermaid(spec))

    def test_draws_the_subgraph_as_a_block(self):
        rendered = render_mermaid(helpers.readme_example_spec())
        self.assertIn("subgraph classify[classify]", rendered)
        self.assertIn("\n  end", rendered)

    def test_draws_boundaries_as_stadium_nodes(self):
        rendered = render_mermaid(helpers.readme_example_spec())
        self.assertIn("([readings])", rendered)
        self.assertIn("([result])", rendered)

    def test_labels_the_inner_edge_with_its_namespaced_topic(self):
        # Which is the whole reason to render topics: the label shows that the
        # subgraph ID namespaced the node.
        self.assertIn(
            '"classify.flag.flagged"', render_mermaid(helpers.readme_example_spec())
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
        self.assertIn("scale --> classify", rendered)


class TestPolicyMarkers(unittest.TestCase):
    def test_non_default_policies_are_drawn_on_the_node(self):
        builder = GraphBuilder("g")
        builder.add("plain", helpers.Double)
        builder.add(
            "marked", helpers.Passthrough, readiness=AnyInput(), on_error="drop"
        )
        builder.connect("plain.output", "marked.inp")
        builder.add_input("source", "plain.inp")
        builder.add_output("output", "marked.output")
        rendered = render_mermaid(builder.build())
        self.assertIn("plain[plain]", rendered)
        self.assertIn("marked[marked<br/>any/drop]", rendered)


class TestUnvalidatedBlueprints(unittest.TestCase):
    def test_a_blueprint_that_does_not_validate_still_renders(self):
        # Renderable before runnable: a diagram is something you can get from a
        # description that is still wrong.
        builder = GraphBuilder("broken")
        builder.add("a", "type.that.is.not.registered")
        builder.connect("a.output", "ghost.inp")
        rendered = render_mermaid(builder.build())
        self.assertIn("a[a]", rendered)
        self.assertIn('a -- "a.output" --> ghost', rendered)


if __name__ == "__main__":
    unittest.main()
