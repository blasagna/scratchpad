"""The three-call lifecycle, including the `setup`-raises path.

This is the part of the contract with the most ways to be subtly wrong, so each
clause of it gets its own test: order, exactly-once, who does and does not get a
`teardown` when `setup` fails, and that `teardown` runs on an error stop too.
"""

import unittest

import helpers
from dfg.blueprint import GraphBuilder
from dfg.errors import LifecycleError, NodeRunError, NodeSetupError
from dfg.graph import Graph
from dfg.message import Message


def phases(trace, phase):
    """The labels that reached ``phase``, in the order they did."""
    return [label for label, seen in trace if seen == phase]


class TestOrder(unittest.TestCase):
    def test_setup_runs_in_topological_order(self):
        trace: list[tuple[str, str]] = []
        spec = helpers.chain_spec(labels=("a", "b", "c"), trace=trace)
        graph = Graph.instantiate(spec, helpers.build_registry())
        graph.start()
        self.assertEqual(phases(trace, "setup"), ["a", "b", "c"])

    def test_teardown_runs_in_exactly_reverse_order(self):
        # A node tears down before anything it depends on.
        trace: list[tuple[str, str]] = []
        spec = helpers.chain_spec(labels=("a", "b", "c"), trace=trace)
        with Graph.instantiate(spec, helpers.build_registry()):
            pass
        self.assertEqual(phases(trace, "setup"), ["a", "b", "c"])
        self.assertEqual(phases(trace, "teardown"), ["c", "b", "a"])

    def test_lifecycle_order_follows_the_wiring_not_the_alphabet(self):
        trace: list[tuple[str, str]] = []
        spec = helpers.chain_spec(labels=("c", "b", "a"), trace=trace)
        with Graph.instantiate(spec, helpers.build_registry()):
            pass
        # The chain is c -> b -> a here, so topological order follows the wiring,
        # not the alphabet and not the declaration order.
        self.assertEqual(phases(trace, "setup"), ["c", "b", "a"])
        self.assertEqual(phases(trace, "teardown"), ["a", "b", "c"])

    def test_each_call_happens_exactly_once(self):
        trace: list[tuple[str, str]] = []
        spec = helpers.chain_spec(labels=("a", "b"), trace=trace)
        graph = Graph.instantiate(spec, helpers.build_registry())
        graph.start()
        graph.inject("source", Message(1, 10))
        graph.run_until_idle()
        graph.stop()
        graph.stop()  # idempotent
        self.assertEqual(phases(trace, "setup"), ["a", "b"])
        self.assertEqual(phases(trace, "teardown"), ["b", "a"])

    def test_run_never_precedes_setup(self):
        trace: list[tuple[str, str]] = []
        spec = helpers.chain_spec(labels=("a", "b"), trace=trace)
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
        first_run = next(i for i, (_, phase) in enumerate(trace) if phase == "run")
        setups = [i for i, (_, phase) in enumerate(trace) if phase == "setup"]
        self.assertTrue(all(i < first_run for i in setups))


class TestInstantiationRunsNothing(unittest.TestCase):
    def test_instantiate_does_not_call_setup(self):
        # A blueprint layer that is cheap to use means you can build and inspect a
        # graph before it acquires anything.
        trace: list[tuple[str, str]] = []
        spec = helpers.chain_spec(labels=("a", "b"), trace=trace)
        graph = Graph.instantiate(spec, helpers.build_registry())
        self.assertEqual(trace, [])
        self.assertFalse(graph.started)
        self.assertEqual(len(graph.nodes), 2)

    def test_stepping_before_start_is_an_error(self):
        graph = Graph.instantiate(helpers.chain_spec(), helpers.build_registry())
        with self.assertRaises(LifecycleError) as caught:
            graph.step()
        self.assertIn("start()", str(caught.exception))

    def test_starting_twice_is_an_error(self):
        graph = Graph.instantiate(helpers.chain_spec(), helpers.build_registry())
        graph.start()
        with self.assertRaises(LifecycleError):
            graph.start()

    def test_stopping_a_graph_that_never_started_is_fine(self):
        trace: list[tuple[str, str]] = []
        graph = Graph.instantiate(
            helpers.chain_spec(trace=trace), helpers.build_registry()
        )
        graph.stop()
        self.assertEqual(trace, [])

    def test_a_stopped_graph_cannot_run_again(self):
        graph = Graph.instantiate(helpers.chain_spec(), helpers.build_registry())
        graph.start()
        graph.stop()
        with self.assertRaises(LifecycleError) as caught:
            graph.run_until_idle()
        self.assertIn("instantiate a new one", str(caught.exception))


class TestSetupRaises(unittest.TestCase):
    def spec(self, trace):
        """a -> raiser -> c, so there is a node before and a node after."""
        builder = GraphBuilder("g")
        builder.add("n_a", helpers.TraceNode, params={"trace": trace, "label": "a"})
        builder.add(
            "n_raiser",
            helpers.RaiseInSetup,
            params={"trace": trace, "label": "raiser"},
        )
        builder.add("n_c", helpers.TraceNode, params={"trace": trace, "label": "c"})
        builder.connect("n_a.output", "n_raiser.input")
        builder.connect("n_raiser.output", "n_c.input")
        builder.add_input("source", "n_a.input")
        builder.add_output("sink", "n_c.output")
        return builder.build()

    def test_the_graph_fails_to_start(self):
        trace: list[tuple[str, str]] = []
        graph = Graph.instantiate(self.spec(trace), helpers.build_registry())
        with self.assertRaises(NodeSetupError) as caught:
            graph.start()
        self.assertIn("n_raiser", str(caught.exception))
        self.assertIsInstance(caught.exception.__cause__, RuntimeError)

    def test_the_raiser_gets_no_teardown(self):
        # It never finished acquiring, so it has no business being asked to release.
        trace: list[tuple[str, str]] = []
        graph = Graph.instantiate(self.spec(trace), helpers.build_registry())
        with self.assertRaises(NodeSetupError):
            graph.start()
        self.assertNotIn(("raiser", "teardown"), trace)

    def test_every_node_already_set_up_does_get_one(self):
        trace: list[tuple[str, str]] = []
        graph = Graph.instantiate(self.spec(trace), helpers.build_registry())
        with self.assertRaises(NodeSetupError):
            graph.start()
        self.assertEqual(phases(trace, "teardown"), ["a"])

    def test_a_node_that_never_ran_setup_never_runs_teardown(self):
        trace: list[tuple[str, str]] = []
        graph = Graph.instantiate(self.spec(trace), helpers.build_registry())
        with self.assertRaises(NodeSetupError):
            graph.start()
        self.assertEqual(phases(trace, "setup"), ["a", "raiser"])
        self.assertNotIn("c", phases(trace, "teardown"))

    def test_already_set_up_nodes_tear_down_in_reverse_order_among_themselves(self):
        trace: list[tuple[str, str]] = []
        builder = GraphBuilder("g")
        for label in ("a", "b"):
            builder.add(
                f"n_{label}", helpers.TraceNode, params={"trace": trace, "label": label}
            )
        builder.add(
            "n_raiser",
            helpers.RaiseInSetup,
            params={"trace": trace, "label": "raiser"},
        )
        builder.connect("n_a.output", "n_b.input")
        builder.connect("n_b.output", "n_raiser.input")
        builder.add_input("source", "n_a.input")
        builder.add_output("sink", "n_raiser.output")
        graph = Graph.instantiate(builder.build(), helpers.build_registry())
        with self.assertRaises(NodeSetupError):
            graph.start()
        self.assertEqual(phases(trace, "teardown"), ["b", "a"])

    def test_the_context_manager_form_reports_the_same_way(self):
        trace: list[tuple[str, str]] = []
        with self.assertRaises(NodeSetupError):
            with Graph.instantiate(self.spec(trace), helpers.build_registry()):
                self.fail("the body must not run")
        self.assertEqual(phases(trace, "teardown"), ["a"])

    def test_node_stats_show_who_was_set_up(self):
        trace: list[tuple[str, str]] = []
        graph = Graph.instantiate(self.spec(trace), helpers.build_registry())
        with self.assertRaises(NodeSetupError):
            graph.start()
        stats = graph.control.node_stats()
        self.assertTrue(stats["n_a"].setup_done)
        self.assertFalse(stats["n_raiser"].setup_done)
        self.assertFalse(stats["n_c"].setup_done)
        self.assertTrue(stats["n_a"].teardown_done)
        self.assertFalse(stats["n_raiser"].teardown_done)


class TestTeardownOnErrorStop(unittest.TestCase):
    def spec(self, trace):
        builder = GraphBuilder("g")
        builder.add("n_a", helpers.TraceNode, params={"trace": trace, "label": "a"})
        builder.add(
            "n_bad", helpers.RaiseInRun, params={"trace": trace, "label": "bad"}
        )
        builder.connect("n_a.output", "n_bad.input")
        builder.add_input("source", "n_a.input")
        builder.add_output("sink", "n_bad.output")
        return builder.build()

    def test_teardown_runs_on_an_error_stop(self):
        # It is the only place a node may assume it will be given.
        trace: list[tuple[str, str]] = []
        graph = Graph.instantiate(self.spec(trace), helpers.build_registry())
        graph.start()
        graph.inject("source", Message(1, 10))
        with self.assertRaises(NodeRunError):
            graph.run_until_idle()
        self.assertEqual(phases(trace, "teardown"), ["bad", "a"])
        self.assertTrue(graph.stopped)

    def test_the_context_manager_tears_down_on_an_exception_in_the_body(self):
        trace: list[tuple[str, str]] = []
        with self.assertRaises(ZeroDivisionError):
            with Graph.instantiate(
                helpers.chain_spec(trace=trace), helpers.build_registry()
            ):
                1 / 0
        self.assertEqual(phases(trace, "teardown"), ["c", "b", "a"])


class TestTeardownFailures(unittest.TestCase):
    def test_every_node_gets_its_turn_even_if_one_raises(self):
        # One node's bad cleanup must not strand another's.
        torn: list[str] = []

        class BadTeardown(helpers.Passthrough):
            def teardown(self):
                torn.append("bad")
                raise RuntimeError("cleanup failed")

        class GoodTeardown(helpers.Passthrough):
            def teardown(self):
                torn.append("good")

        registry = helpers.build_registry()
        registry.register(BadTeardown)
        registry.register(GoodTeardown)

        builder = GraphBuilder("g")
        builder.add("n_good", GoodTeardown)
        builder.add("n_bad", BadTeardown)
        builder.connect("n_good.output", "n_bad.input")
        builder.add_input("source", "n_good.input")
        builder.add_output("sink", "n_bad.output")

        graph = Graph.instantiate(builder.build(), registry)
        graph.start()
        with self.assertRaises(ExceptionGroup):
            graph.stop()
        self.assertEqual(torn, ["bad", "good"])

    def test_a_failing_teardown_does_not_mask_the_error_that_caused_the_stop(self):
        # The node that actually broke is what the caller needs to see; a cleanup
        # failure discovered on the way out is secondary, and only logged.
        class BadTeardown(helpers.Passthrough):
            def teardown(self):
                raise RuntimeError("cleanup failed")

        registry = helpers.build_registry()
        registry.register(BadTeardown)

        builder = GraphBuilder("g")
        builder.add("n_clean", BadTeardown)
        builder.add("n_bad", helpers.RaiseInRun, params={"trace": None, "label": "bad"})
        builder.connect("n_clean.output", "n_bad.input")
        builder.add_input("source", "n_clean.input")
        builder.add_output("sink", "n_bad.output")

        graph = Graph.instantiate(builder.build(), registry)
        graph.start()
        graph.inject("source", Message(1, 10))
        with self.assertLogs("dfg", level="ERROR"):
            with self.assertRaises(NodeRunError):
                graph.run_until_idle()
        self.assertTrue(graph.stopped)

    def test_a_failing_teardown_does_not_mask_a_setup_failure_either(self):
        class BadTeardown(helpers.Passthrough):
            def teardown(self):
                raise RuntimeError("cleanup failed")

        registry = helpers.build_registry()
        registry.register(BadTeardown)

        builder = GraphBuilder("g")
        builder.add("n_first", BadTeardown)
        builder.add("n_raiser", helpers.RaiseInSetup, params={"trace": None})
        builder.connect("n_first.output", "n_raiser.input")
        builder.add_input("source", "n_first.input")
        builder.add_output("sink", "n_raiser.output")

        graph = Graph.instantiate(builder.build(), registry)
        with self.assertLogs("dfg", level="ERROR"):
            with self.assertRaises(NodeSetupError):
                graph.start()


if __name__ == "__main__":
    unittest.main()
