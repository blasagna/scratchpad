"""The three error policies, and why `stop` is the default."""

import unittest
from typing import NamedTuple

import helpers
from dfg.blueprint import GraphBuilder
from dfg.errors import EdgeOverflowError, NodeRunError
from dfg.graph import Graph
from dfg.message import Message
from dfg.node import Emit, In, Node
from dfg.ports import ERROR_PORT
from dfg.scheduler import ErrorEvent


class FailOnOdd(Node):
    """Raises for odd payloads, passes even ones through."""

    class Out(NamedTuple):
        output: Emit[int]

    def run(self, *, inp: In[int] = ()) -> Out:
        out = []
        for message in inp:
            if message.payload % 2:
                raise ValueError(f"cannot handle {message.payload}")
            out.append(message)
        return self.Out(output=tuple(out))


def registry():
    reg = helpers.build_registry()
    reg.register(FailOnOdd)
    return reg


def spec(policy):
    builder = GraphBuilder("g")
    builder.add("picky", FailOnOdd, on_error=policy)
    builder.add("after", helpers.Passthrough)
    builder.connect("picky.output", "after.inp")
    builder.add_input("source", "picky.inp")
    builder.add_output("output", "after.output")
    return builder.build()


class TestStop(unittest.TestCase):
    def test_stop_is_the_default(self):
        self.assertEqual(helpers.node_spec(spec("stop")).on_error, "stop")
        builder = GraphBuilder("g")
        builder.add("n", helpers.Passthrough)
        self.assertEqual(helpers.node_spec(builder.build()).on_error, "stop")

    def test_the_graph_stops_and_the_error_is_chained(self):
        # Silent-continue hides bugs in exactly the case where nobody is watching:
        # an offline batch run over a large recording.
        with Graph.instantiate(spec("stop"), registry()) as graph:
            graph.inject("source", Message(1, 10))
            with self.assertRaises(NodeRunError) as caught:
                graph.run_until_idle()
        self.assertIn("'picky'", str(caught.exception))
        self.assertIn("ValueError", str(caught.exception))
        self.assertIsInstance(caught.exception.__cause__, ValueError)

    def test_teardown_still_runs(self):
        trace: list[tuple[str, str]] = []
        builder = GraphBuilder("g")
        builder.add(
            "before", helpers.TraceNode, params={"trace": trace, "label": "before"}
        )
        builder.add("picky", FailOnOdd)
        builder.connect("before.output", "picky.inp")
        builder.add_input("source", "before.inp")
        builder.add_output("output", "picky.output")
        graph = Graph.instantiate(builder.build(), registry())
        graph.start()
        graph.inject("source", Message(1, 10))
        with self.assertRaises(NodeRunError):
            graph.run_until_idle()
        self.assertIn(("before", "teardown"), trace)
        self.assertTrue(graph.stopped)

    def test_the_error_is_counted(self):
        graph = Graph.instantiate(spec("stop"), registry())
        graph.start()
        graph.inject("source", Message(1, 10))
        with self.assertRaises(NodeRunError):
            graph.run_until_idle()
        stats = graph.control.node_stats()
        self.assertEqual(stats["picky"].errors, 1)
        self.assertEqual(stats["picky"].fired, 0)


class TestDrop(unittest.TestCase):
    def test_the_graph_continues_and_the_bad_message_is_gone(self):
        with Graph.instantiate(spec("drop"), registry()) as graph:
            for i in range(4):
                graph.inject("source", Message(i, i))
            with self.assertLogs("dfg", level="ERROR"):
                graph.run_until_idle()
            self.assertEqual(helpers.payloads(graph.poll("output")), [0, 2])
            self.assertEqual(graph.control.total_pending(), 0)

    def test_the_failure_is_logged_with_a_traceback(self):
        with Graph.instantiate(spec("drop"), registry()) as graph:
            graph.inject("source", Message(1, 10))
            with self.assertLogs("dfg", level="ERROR") as logs:
                graph.run_until_idle()
        self.assertIn("dropped a message", logs.output[0])
        self.assertIn("ValueError", logs.output[0])

    def test_errors_are_counted_and_firings_are_not(self):
        with Graph.instantiate(spec("drop"), registry()) as graph:
            for i in range(4):
                graph.inject("source", Message(i, i))
            with self.assertLogs("dfg", level="ERROR"):
                graph.run_until_idle()
            stats = graph.control.node_stats()
            self.assertEqual(stats["picky"].errors, 2)
            self.assertEqual(stats["picky"].fired, 2)

    def test_downstream_nodes_never_see_the_dropped_message(self):
        with Graph.instantiate(spec("drop"), registry()) as graph:
            graph.inject("source", Message(1, 10))
            with self.assertLogs("dfg", level="ERROR"):
                graph.run_until_idle()
            self.assertEqual(graph.control.node_stats()["after"].fired, 0)


class TestRoute(unittest.TestCase):
    def test_the_error_is_published_on_the_nodes_error_topic(self):
        seen: list[tuple[str, ErrorEvent]] = []
        with Graph.instantiate(spec("route"), registry()) as graph:
            graph.subscribe_errors(
                "picky", lambda name, m: seen.append((name, m.payload))
            )
            graph.inject("source", Message(1, 10))
            with self.assertLogs("dfg", level="WARNING"):
                graph.run_until_idle()
        self.assertEqual(len(seen), 1)
        name, event = seen[0]
        self.assertEqual(name, f"picky.{ERROR_PORT}")
        self.assertEqual(event.qid, "picky")
        self.assertEqual(event.exception_type, "ValueError")
        self.assertIn("cannot handle 1", event.message)

    def test_the_graph_continues(self):
        with Graph.instantiate(spec("route"), registry()) as graph:
            for i in range(4):
                graph.inject("source", Message(i, i))
            with self.assertLogs("dfg", level="WARNING"):
                graph.run_until_idle()
            self.assertEqual(helpers.payloads(graph.poll("output")), [0, 2])

    def test_the_error_topic_has_no_edges(self):
        # An error is observed, never routed into another node's input port -- so
        # there is no edge for it and nothing downstream can consume it.
        with Graph.instantiate(spec("route"), registry()) as graph:
            self.assertNotIn(f"picky.{ERROR_PORT}", graph.topics)
            self.assertFalse(
                any(ERROR_PORT in key for key in graph.control.edge_keys())
            )

    def test_subscribing_to_an_unknown_node_is_rejected(self):
        with Graph.instantiate(spec("route"), registry()) as graph:
            with self.assertRaises(KeyError):
                graph.subscribe_errors("ghost", lambda name, m: None)

    def test_an_error_event_carries_a_wall_clock_stamped_message(self):
        seen: list[Message] = []
        clock = helpers.FakeClock(start=500, step=0)
        with Graph.instantiate(spec("route"), registry(), clock=clock) as graph:
            graph.subscribe_errors("picky", lambda name, m: seen.append(m))
            graph.inject("source", Message(1, 10**15))
            with self.assertLogs("dfg", level="WARNING"):
                graph.run_until_idle()
        # The event's own timestamp is the control clock, not the sample time of
        # the message that caused it.
        self.assertEqual(seen[0].timestamp, 500)


class TestEdgeOverflowIsNotANodeError(unittest.TestCase):
    def test_an_overflowing_edge_stops_the_graph_whatever_the_node_policy(self):
        # The node did nothing wrong, so its error policy has no say.
        builder = GraphBuilder("g")
        builder.add("fan", helpers.EmitN, params={"n": 5}, on_error="drop")
        builder.add("after", helpers.Passthrough)
        builder.connect("fan.output", "after.inp", capacity=2, on_overflow="error")
        builder.add_input("source", "fan.inp")
        builder.add_output("output", "after.output")
        graph = Graph.instantiate(builder.build(), registry())
        graph.start()
        graph.inject("source", Message("x", 10))
        with self.assertRaises(EdgeOverflowError):
            graph.run_until_idle()
        self.assertTrue(graph.stopped)

    def test_a_dropping_edge_keeps_going_and_counts_the_drops(self):
        builder = GraphBuilder("g")
        builder.add("fan", helpers.EmitN, params={"n": 5})
        builder.add("after", helpers.Passthrough)
        builder.connect(
            "fan.output", "after.inp", capacity=2, on_overflow="drop_oldest"
        )
        builder.add_input("source", "fan.inp")
        builder.add_output("output", "after.output")
        with Graph.instantiate(builder.build(), registry()) as graph:
            graph.inject("source", Message("x", 10))
            graph.run_until_idle()
            stats = graph.control.edge_stats()["fan.output -> after.inp"]
            self.assertEqual(stats.dropped, 3)
            self.assertEqual(len(graph.poll("output")), 2)


if __name__ == "__main__":
    unittest.main()
