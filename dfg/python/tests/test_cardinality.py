"""Output cardinality: zero or more, and one-in-one-out is not the contract.

Decimation emits nothing on most invocations and framing a stream into overlapping
windows emits several. An API that returned one value per output port could express
neither, and audio and video are mostly these.
"""

import unittest
from typing import Any, NamedTuple

import helpers
from dfg.blueprint import GraphBuilder
from dfg.errors import NodeContractError
from dfg.graph import Graph
from dfg.message import Message
from dfg.node import Emit, In, Node


def one_node_graph(type_name, *, params=None, registry=None):
    builder = GraphBuilder("g")
    builder.add("n", type_name, params=params or {})
    builder.add_input("source", "n.inp")
    builder.add_output("output", "n.output")
    return Graph.instantiate(builder.build(), registry or helpers.build_registry())


class TestZero(unittest.TestCase):
    def test_none_produces_nothing(self):
        with one_node_graph(helpers.EmitNothing) as graph:
            graph.inject("source", Message(1, 10))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(graph.poll("output"), ())

    def test_an_empty_mapping_produces_nothing(self):
        with one_node_graph(helpers.EmitEmptyMapping) as graph:
            graph.inject("source", Message(1, 10))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(graph.poll("output"), ())

    def test_an_empty_port_produces_nothing(self):
        with one_node_graph(helpers.EmitEmptyPort) as graph:
            graph.inject("source", Message(1, 10))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(graph.poll("output"), ())

    def test_a_node_that_produces_nothing_still_counts_as_fired(self):
        # The node ran; it just had nothing to say. Conflating the two would make a
        # decimator look broken in the stats.
        with one_node_graph(helpers.EmitNothing) as graph:
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
            self.assertEqual(graph.control.node_stats()["n"].fired, 1)

    def test_nothing_downstream_fires_either(self):
        builder = GraphBuilder("g")
        builder.add("silent", helpers.EmitNothing)
        builder.add("after", helpers.Passthrough)
        builder.connect("silent.output", "after.inp")
        builder.add_input("source", "silent.inp")
        builder.add_output("output", "after.output")
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 10))
            self.assertEqual(graph.run_until_idle(), 1)
            self.assertEqual(graph.control.node_stats()["after"].fired, 0)


class TestMany(unittest.TestCase):
    def test_three_messages_arrive_in_order(self):
        with one_node_graph(helpers.EmitN, params={"n": 3}) as graph:
            graph.inject("source", Message("x", 10))
            graph.run_until_idle()
            self.assertEqual(
                helpers.payloads(graph.poll("output")),
                [("x", 0), ("x", 1), ("x", 2)],
            )

    def test_every_emitted_message_keeps_the_sample_time(self):
        with one_node_graph(helpers.EmitN, params={"n": 3}) as graph:
            graph.inject("source", Message("x", 4242))
            graph.run_until_idle()
            self.assertEqual(
                [m.timestamp for m in graph.poll("output")], [4242, 4242, 4242]
            )

    def test_all_of_them_reach_a_downstream_node(self):
        builder = GraphBuilder("g")
        builder.add("fan", helpers.EmitN, params={"n": 4})
        builder.add("after", helpers.Passthrough)
        builder.connect("fan.output", "after.inp")
        builder.add_input("source", "fan.inp")
        builder.add_output("output", "after.output")
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            graph.inject("source", Message("x", 10))
            # One firing of `fan`, then one of `after` per message it produced.
            self.assertEqual(graph.run_until_idle(), 5)
            self.assertEqual(len(graph.poll("output")), 4)


class TestDecimation(unittest.TestCase):
    def test_a_decimator_emits_on_every_nth_firing_only(self):
        class Decimate(Node):
            factor: int = 3

            class Out(NamedTuple):
                output: Emit[Any]

            def setup(self) -> None:
                self._seen = 0

            def run(self, *, inp: In[Any] = ()) -> Out:
                out = []
                for message in inp:
                    self._seen += 1
                    if self._seen % self.factor == 0:
                        out.append(message)
                return self.Out(output=tuple(out))

        registry = helpers.build_registry()
        registry.register(Decimate)
        with one_node_graph(Decimate, params={"factor": 3}, registry=registry) as graph:
            for i in range(7):
                graph.inject("source", Message(i, i))
            self.assertEqual(graph.run_until_idle(), 7)
            self.assertEqual(helpers.payloads(graph.poll("output")), [2, 5])


class TestContractViolations(unittest.TestCase):
    def test_a_bare_message_is_rejected_at_run_time(self):
        with one_node_graph(helpers.ReturnBareMessage) as graph:
            graph.inject("source", Message(1, 10))
            with self.assertRaises(NodeContractError) as caught:
                graph.run_until_idle()
            self.assertIn("n.run returned a bare Message", str(caught.exception))

    def test_an_unknown_output_port_is_rejected(self):
        with one_node_graph(helpers.ReturnUnknownPort) as graph:
            graph.inject("source", Message(1, 10))
            with self.assertRaises(NodeContractError) as caught:
                graph.run_until_idle()
            self.assertIn("nope", str(caught.exception))

    def test_a_contract_violation_still_tears_the_graph_down(self):
        # It is a bug in the node, not a bad message, so no error policy applies --
        # but teardown is still promised.
        graph = one_node_graph(helpers.ReturnBareMessage)
        graph.start()
        graph.inject("source", Message(1, 10))
        with self.assertRaises(NodeContractError):
            graph.run_until_idle()
        self.assertTrue(graph.stopped)


if __name__ == "__main__":
    unittest.main()
