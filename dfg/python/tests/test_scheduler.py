"""Readiness rules, orderings, and the one place ties break."""

import unittest

import helpers
from dfg.blueprint import GraphBuilder
from dfg.errors import LifecycleError
from dfg.flatten import flatten
from dfg.graph import Graph
from dfg.message import Message
from dfg.ordering import (
    LevelOrdering,
    PriorityOrdering,
    TopologicalOrdering,
    make_ordering,
    ordering_names,
    register_ordering,
)
from dfg.readiness import (
    AllInputs,
    AnyInput,
    CountAtLeast,
    PredicateRule,
    readiness_kinds,
)


def fired(trace):
    """Node labels in the order they ran."""
    return [label for label, phase in trace if phase == "run"]


class TestReadinessRules(unittest.TestCase):
    def join_spec(self, rule):
        builder = GraphBuilder("g")
        builder.add("join", "t.sum2", readiness=rule)
        builder.add_input("left", "join.a")
        builder.add_input("right", "join.b")
        builder.add_output("out", "join.out")
        return builder.build()

    def test_all_waits_for_every_port(self):
        with Graph.instantiate(
            self.join_spec(AllInputs()), helpers.build_registry()
        ) as graph:
            graph.inject("left", Message(1, 10))
            self.assertEqual(graph.run_until_idle(), 0)
            self.assertEqual(graph.poll("out"), ())
            graph.inject("right", Message(2, 20))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(helpers.payloads(graph.poll("out")), [3])

    def test_any_fires_on_one_port(self):
        with Graph.instantiate(
            self.join_spec(AnyInput()), helpers.build_registry()
        ) as graph:
            graph.inject("left", Message(1, 10))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(helpers.payloads(graph.poll("out")), [1])

    def test_any_takes_every_available_port_when_both_have_messages(self):
        with Graph.instantiate(
            self.join_spec(AnyInput()), helpers.build_registry()
        ) as graph:
            graph.inject("left", Message(1, 10))
            graph.inject("right", Message(2, 20))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(helpers.payloads(graph.poll("out")), [3])

    def test_default_consumption_is_one_message_per_port(self):
        with Graph.instantiate(
            self.join_spec(AllInputs()), helpers.build_registry()
        ) as graph:
            for i in range(3):
                graph.inject("left", Message(1, i))
                graph.inject("right", Message(1, i))
            self.assertEqual(graph.run_until_idle(), 3)
            self.assertEqual(helpers.payloads(graph.poll("out")), [2, 2, 2])

    def test_count_batches_and_consumes_the_whole_batch(self):
        # This is how "a batch is a very large sample" gets expressed: one rule,
        # not a second engine.
        builder = GraphBuilder("g")
        builder.add("batch", "t.emit_n", params={"n": 1}, readiness=CountAtLeast(4))
        builder.add_input("source", "batch.in")
        builder.add_output("out", "batch.out")
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            for i in range(6):
                graph.inject("source", Message(i, i))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(len(graph.poll("out")), 4)
            self.assertEqual(graph.control.total_pending(), 2)

    def test_count_on_a_named_port_takes_one_from_the_others(self):
        builder = GraphBuilder("g")
        builder.add("join", "t.sum2", readiness=CountAtLeast(3, port="a"))
        builder.add_input("left", "join.a")
        builder.add_input("right", "join.b")
        builder.add_output("out", "join.out")
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            for _ in range(3):
                graph.inject("left", Message(1, 0))
            for _ in range(2):
                graph.inject("right", Message(10, 0))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(helpers.payloads(graph.poll("out")), [13])
            self.assertEqual(graph.control.total_pending(), 1)

    def test_count_rejects_a_non_positive_count(self):
        with self.assertRaises(ValueError):
            CountAtLeast(0)

    def test_a_predicate_rule_drives_firing(self):
        # Fire only once two messages have piled up on the single port.
        rule = PredicateRule(lambda queues: len(queues["in"]) >= 2, name="two_buffered")
        builder = GraphBuilder("g")
        builder.add("head", "t.passthrough", readiness=rule)
        builder.add_input("source", "head.in")
        builder.add_output("out", "head.out")
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 1))
            self.assertEqual(graph.run_until_idle(), 0)
            graph.inject("source", Message(2, 2))
            self.assertEqual(graph.run_until_idle(), 1)

    def test_all_refuses_to_fire_a_node_with_no_ports(self):
        # Vacuous truth would fire it forever. Validation rejects such nodes, but
        # the rule does not depend on that.
        self.assertFalse(AllInputs().is_ready({}))

    def test_the_built_in_kinds_are_registered_and_predicate_is_not(self):
        self.assertEqual(readiness_kinds(), ("all", "any", "count"))


class TestOrderings(unittest.TestCase):
    def fire_with(self, ordering, *, side_priority=0):
        trace: list[tuple[str, str]] = []
        spec = helpers.scheduling_spec(trace, side_priority=side_priority)
        with Graph.instantiate(
            spec, helpers.build_registry(), ordering=ordering
        ) as graph:
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
        return fired(trace)

    def test_topological_follows_the_sort(self):
        self.assertEqual(
            self.fire_with(TopologicalOrdering()),
            ["head", "chain1", "chain2", "chain3", "side"],
        )

    def test_level_goes_breadth_first(self):
        # After chain1, `side` is at level 1 and chain2 at level 2, so side runs
        # first -- which is exactly where this ordering differs from topological.
        self.assertEqual(
            self.fire_with(LevelOrdering()),
            ["head", "chain1", "side", "chain2", "chain3"],
        )

    def test_priority_puts_the_author_in_charge(self):
        self.assertEqual(
            self.fire_with(PriorityOrdering(), side_priority=10),
            ["head", "side", "chain1", "chain2", "chain3"],
        )

    def test_two_orderings_produce_the_same_outputs(self):
        # Different firing orders, same result. That is what makes swapping the
        # scheduler a policy change rather than a behaviour change.
        outputs = []
        for ordering in ("topological", "level"):
            spec = helpers.scheduling_spec()
            with Graph.instantiate(
                spec, helpers.build_registry(), ordering=ordering
            ) as graph:
                graph.inject("source", Message(1, 10))
                graph.run_until_idle()
                outputs.append(
                    {
                        name: helpers.digest(messages)
                        for name, messages in graph.poll_all().items()
                    }
                )
        self.assertEqual(outputs[0], outputs[1])

    def test_an_ordering_can_be_named(self):
        self.assertEqual(ordering_names(), ("level", "priority", "topological"))
        self.assertIsInstance(make_ordering("level"), LevelOrdering)
        with self.assertRaises(ValueError):
            make_ordering("random")

    def test_registering_an_ordering(self):
        class Reverse(TopologicalOrdering):
            NAME = "test_only_reverse"

        register_ordering(Reverse)
        try:
            self.assertIsInstance(make_ordering("test_only_reverse"), Reverse)
            with self.assertRaises(ValueError):
                register_ordering(type("Other", (Reverse,), {}))
        finally:
            from dfg import ordering as ordering_module

            del ordering_module._ORDERINGS["test_only_reverse"]

    def test_an_ordering_without_a_name_cannot_be_registered(self):
        class Nameless(TopologicalOrdering):
            NAME = ""

        with self.assertRaises(ValueError):
            register_ordering(Nameless)


class TestTieBreaking(unittest.TestCase):
    """The contract's most easily-broken clause, checked from two directions."""

    def symmetric_spec(self, trace):
        """Two structurally identical nodes, declared b-then-a on purpose."""
        builder = GraphBuilder("g")
        builder.add("twin_b", "t.trace", params={"trace": trace, "label": "b"})
        builder.add("twin_a", "t.trace", params={"trace": trace, "label": "a"})
        builder.add("join", "t.sum2")
        builder.connect("twin_b.out", "join.b")
        builder.connect("twin_a.out", "join.a")
        builder.add_input("left", "twin_b.in")
        builder.add_input("right", "twin_a.in")
        builder.add_output("out", "join.out")
        return builder.build()

    def test_every_key_ends_with_the_qualified_id(self):
        # The rule that makes `min(ready, key=ordering.key)` deterministic. An
        # ordering that forgets it fails here rather than corrupting a replay.
        flat = flatten(helpers.scheduling_spec(), helpers.build_registry())
        for ordering in (TopologicalOrdering(), LevelOrdering(), PriorityOrdering()):
            ordering.prepare(flat)
            for qid in flat.nodes:
                with self.subTest(ordering=type(ordering).__name__, qid=qid):
                    self.assertEqual(ordering.key(qid)[-1], qid)

    def test_the_lower_qualified_id_fires_first_under_every_ordering(self):
        for name in ordering_names():
            with self.subTest(ordering=name):
                trace: list[tuple[str, str]] = []
                spec = self.symmetric_spec(trace)
                registry = helpers.build_registry()
                with Graph.instantiate(spec, registry, ordering=name) as graph:
                    graph.inject("left", Message(1, 10))
                    graph.inject("right", Message(2, 20))
                    graph.run_until_idle()
                self.assertEqual(fired(trace)[:2], ["a", "b"])

    def test_the_ready_set_is_itself_in_canonical_order(self):
        spec = helpers.scheduling_spec()
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 10))
            graph.step()  # head fires, so chain1 and side are both ready
            self.assertEqual(graph.ready(), ["chain1", "side"])


class TestSteppingAndQuiescence(unittest.TestCase):
    def test_step_fires_one_node_and_reports_whether_it_did(self):
        spec = helpers.chain_spec(labels=("a", "b"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            self.assertFalse(graph.step())
            graph.inject("source", Message(1, 10))
            self.assertTrue(graph.step())
            self.assertTrue(graph.step())
            self.assertFalse(graph.step())

    def test_run_until_idle_returns_the_step_count(self):
        spec = helpers.chain_spec(labels=("a", "b", "c"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 10))
            self.assertEqual(graph.run_until_idle(), 3)
            self.assertEqual(graph.run_until_idle(), 0)
            self.assertEqual(graph.control.total_pending(), 1)  # the output sink

    def test_quiescence_means_nothing_is_ready(self):
        spec = helpers.chain_spec(labels=("a", "b"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
            self.assertEqual(graph.ready(), [])

    def test_max_steps_guards_a_runaway(self):
        spec = helpers.chain_spec(labels=("a", "b", "c"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 10))
            with self.assertRaises(LifecycleError) as caught:
                graph.run_until_idle(max_steps=1)
            self.assertIn("max_steps=1", str(caught.exception))

    def test_max_steps_is_not_hit_when_the_graph_finishes_first(self):
        spec = helpers.chain_spec(labels=("a", "b"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 10))
            self.assertEqual(graph.run_until_idle(max_steps=2), 2)


if __name__ == "__main__":
    unittest.main()
