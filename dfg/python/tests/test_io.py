"""Injecting, polling, callbacks, and topic taps.

Requirement 9 offers two ways to get data out, and they are fed from one publish
step so they can never disagree about what a run produced. Requirement 10's topics
are the third way, and they are a tap rather than a bus: subscribing observes a
port while the messages still travel over each edge's own transport.
"""

import unittest

import helpers
from dfg.blueprint import GraphBuilder
from dfg.graph import Graph, iter_messages
from dfg.message import Message


class TestInjectAndPoll(unittest.TestCase):
    def graph(self):
        return Graph.instantiate(
            helpers.chain_spec(labels=("a", "b")), helpers.build_registry()
        )

    def test_inject_then_poll(self):
        with self.graph() as graph:
            graph.inject("source", Message(7, 100))
            graph.run_until_idle()
            (message,) = graph.poll("sink")
            self.assertEqual(message.payload, 7)
            self.assertEqual(message.timestamp, 100)

    def test_inject_payload_is_the_same_thing(self):
        with self.graph() as graph:
            graph.inject_payload("source", 7, 100)
            graph.run_until_idle()
            self.assertEqual(helpers.digest(graph.poll("sink")), [(7, 100)])

    def test_poll_drains(self):
        with self.graph() as graph:
            for i in range(3):
                graph.inject("source", Message(i, i))
            graph.run_until_idle()
            self.assertEqual(len(graph.poll("sink")), 3)
            self.assertEqual(graph.poll("sink"), ())

    def test_poll_all_covers_every_output_in_declaration_order(self):
        with Graph.instantiate(
            helpers.scheduling_spec(), helpers.build_registry()
        ) as graph:
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
            polled = graph.poll_all()
            self.assertEqual(list(polled), ["chained", "aside"])
            self.assertEqual(len(polled["chained"]), 1)
            self.assertEqual(len(polled["aside"]), 1)

    def test_an_unknown_input_or_output_is_a_key_error(self):
        with self.graph() as graph:
            with self.assertRaises(KeyError):
                graph.inject("nope", Message(1, 1))
            with self.assertRaises(KeyError):
                graph.poll("nope")

    def test_names_are_reported(self):
        with self.graph() as graph:
            self.assertEqual(graph.input_names, ("source",))
            self.assertEqual(graph.output_names, ("sink",))

    def test_injecting_into_a_fan_out_input_reaches_every_target(self):
        builder = GraphBuilder("g")
        builder.add("left", helpers.Double)
        builder.add("right", helpers.Double)
        builder.add_input("shared", "left.input", "right.input")
        builder.add_output("l", "left.output")
        builder.add_output("r", "right.output")
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            graph.inject("shared", Message(3, 10))
            self.assertEqual(graph.run_until_idle(), 2)
            self.assertEqual(helpers.payloads(graph.poll("l")), [6])
            self.assertEqual(helpers.payloads(graph.poll("r")), [6])

    def test_iter_messages_flattens_a_poll_result(self):
        with Graph.instantiate(
            helpers.scheduling_spec(), helpers.build_registry()
        ) as graph:
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
            pairs = list(iter_messages(graph.poll_all()))
            self.assertEqual([name for name, _ in pairs], ["chained", "aside"])


class TestOutputCallbacks(unittest.TestCase):
    def test_a_callback_fires_as_output_appears(self):
        seen: list[tuple[str, int]] = []
        with Graph.instantiate(
            helpers.chain_spec(labels=("a", "b")), helpers.build_registry()
        ) as graph:
            graph.on_output("sink", lambda name, m: seen.append((name, m.payload)))
            for i in range(3):
                graph.inject("source", Message(i, i))
            graph.run_until_idle()
        self.assertEqual(seen, [("sink", 0), ("sink", 1), ("sink", 2)])

    def test_a_callback_does_not_consume_the_message(self):
        # Poll and callback are two views of one publish step, not two channels.
        seen: list[int] = []
        with Graph.instantiate(
            helpers.chain_spec(labels=("a",)), helpers.build_registry()
        ) as graph:
            graph.on_output("sink", lambda name, m: seen.append(m.payload))
            graph.inject("source", Message(5, 10))
            graph.run_until_idle()
            self.assertEqual(seen, [5])
            self.assertEqual(helpers.payloads(graph.poll("sink")), [5])

    def test_several_callbacks_fire_in_registration_order(self):
        seen: list[str] = []
        with Graph.instantiate(
            helpers.chain_spec(labels=("a",)), helpers.build_registry()
        ) as graph:
            graph.on_output("sink", lambda name, m: seen.append("first"))
            graph.on_output("sink", lambda name, m: seen.append("second"))
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
        self.assertEqual(seen, ["first", "second"])

    def test_registering_on_an_unknown_output_is_rejected(self):
        with Graph.instantiate(helpers.chain_spec(), helpers.build_registry()) as graph:
            with self.assertRaises(KeyError):
                graph.on_output("nope", lambda name, m: None)


class TestTaps(unittest.TestCase):
    def test_a_tap_on_a_fanned_out_port_sees_each_message_exactly_once(self):
        # Fan-out from one output is still ONE topic: a topic names the output
        # port, not each edge leaving it.
        seen: list[int] = []
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double)
        builder.add("left", helpers.Passthrough)
        builder.add("right", helpers.Passthrough)
        builder.connect("head.output", "left.input")
        builder.connect("head.output", "right.input")
        builder.add_input("source", "head.input")
        builder.add_output("l", "left.output")
        builder.add_output("r", "right.output")
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            graph.subscribe("head.output", lambda name, m: seen.append(m.payload))
            graph.inject("source", Message(3, 10))
            graph.run_until_idle()
            # Both consumers got it...
            self.assertEqual(helpers.payloads(graph.poll("l")), [6])
            self.assertEqual(helpers.payloads(graph.poll("r")), [6])
        # ...and the subscriber saw it once.
        self.assertEqual(seen, [6])

    def test_an_alias_and_the_aliased_topic_receive_identical_messages(self):
        # "Subscribing to an alias and subscribing to the aliased topic observe the
        # same output port and the same messages."
        via_alias: list[Message] = []
        via_topic: list[Message] = []
        with Graph.instantiate(
            helpers.readme_example_spec(), helpers.build_registry()
        ) as graph:
            graph.subscribe("fusion.pose", lambda name, m: via_alias.append(m))
            graph.subscribe("fusion.update.fused", lambda name, m: via_topic.append(m))
            graph.inject("imu_raw", Message("imu", 1000))
            graph.inject("frames", Message("frame", 1000))
            graph.run_until_idle()
        self.assertEqual(len(via_alias), 1)
        self.assertEqual(via_alias, via_topic)

    def test_the_root_output_alias_is_subscribable_too(self):
        seen: list[Message] = []
        with Graph.instantiate(
            helpers.readme_example_spec(), helpers.build_registry()
        ) as graph:
            graph.subscribe("pose", lambda name, m: seen.append(m))
            graph.inject("imu_raw", Message("imu", 1000))
            graph.inject("frames", Message("frame", 1000))
            graph.run_until_idle()
            polled = graph.poll("pose")
        self.assertEqual(list(polled), seen)

    def test_the_subscribed_name_is_what_the_callback_is_told(self):
        names: list[str] = []
        with Graph.instantiate(
            helpers.readme_example_spec(), helpers.build_registry()
        ) as graph:
            graph.subscribe("fusion.pose", lambda name, m: names.append(name))
            graph.subscribe("fusion.update.fused", lambda name, m: names.append(name))
            graph.inject("imu_raw", Message("imu", 1000))
            graph.inject("frames", Message("frame", 1000))
            graph.run_until_idle()
        self.assertEqual(sorted(names), ["fusion.pose", "fusion.update.fused"])

    def test_cancel_stops_delivery_and_is_idempotent(self):
        seen: list[int] = []
        with Graph.instantiate(
            helpers.chain_spec(labels=("a",)), helpers.build_registry()
        ) as graph:
            subscription = graph.subscribe(
                "n_a.output", lambda name, m: seen.append(m.payload)
            )
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
            subscription.cancel()
            subscription.cancel()
            self.assertTrue(subscription.cancelled)
            graph.inject("source", Message(2, 20))
            graph.run_until_idle()
        self.assertEqual(seen, [1])

    def test_a_subscription_works_as_a_context_manager(self):
        seen: list[int] = []
        with Graph.instantiate(
            helpers.chain_spec(labels=("a",)), helpers.build_registry()
        ) as graph:
            with graph.subscribe("n_a.output", lambda name, m: seen.append(m.payload)):
                graph.inject("source", Message(1, 10))
                graph.run_until_idle()
            graph.inject("source", Message(2, 20))
            graph.run_until_idle()
        self.assertEqual(seen, [1])

    def test_an_unknown_topic_lists_the_alternatives(self):
        with Graph.instantiate(
            helpers.readme_example_spec(), helpers.build_registry()
        ) as graph:
            with self.assertRaises(KeyError) as caught:
                graph.subscribe("nope.nothing", lambda name, m: None)
            self.assertIn("calib.corrected", str(caught.exception))
            self.assertIn("fusion.pose", str(caught.exception))

    def test_a_tap_does_not_move_data(self):
        # Messages travel over each edge's own transport whether anyone is
        # subscribed or not; a tap is something you can decline to install.
        def run(subscribe):
            spec = helpers.chain_spec(labels=("a", "b"))
            with Graph.instantiate(spec, helpers.build_registry()) as graph:
                if subscribe:
                    graph.subscribe("n_a.output", lambda name, m: None)
                graph.inject("source", Message(1, 10))
                graph.run_until_idle()
                return helpers.digest(graph.poll("sink"))

        self.assertEqual(run(subscribe=False), run(subscribe=True))


class TestPublishOrder(unittest.TestCase):
    def test_ports_publish_in_declared_order_not_returned_order(self):
        from typing import Any, NamedTuple

        from dfg.node import Emit, In, Node

        class TwoOut(Node):
            class Out(NamedTuple):
                first: Emit[Any]
                second: Emit[Any]

            def run(self, *, input: In[Any] = ()) -> Out:
                # Deliberately built in the wrong order.
                return self.Out(second=input, first=input)

        registry = helpers.build_registry()
        registry.register(TwoOut)
        seen: list[str] = []
        builder = GraphBuilder("g")
        builder.add("n", TwoOut)
        builder.add_input("source", "n.input")
        builder.add_output("a", "n.first")
        builder.add_output("b", "n.second")
        with Graph.instantiate(builder.build(), registry) as graph:
            graph.subscribe("n.first", lambda name, m: seen.append(name))
            graph.subscribe("n.second", lambda name, m: seen.append(name))
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
        self.assertEqual(seen, ["n.first", "n.second"])

    def test_two_outputs_may_alias_the_same_port(self):
        builder = GraphBuilder("g")
        builder.add("n", helpers.Double)
        builder.add_input("source", "n.input")
        builder.add_output("primary", "n.output")
        builder.add_output("copy", "n.output")
        seen: list[int] = []
        with Graph.instantiate(builder.build(), helpers.build_registry()) as graph:
            graph.subscribe("n.output", lambda name, m: seen.append(m.payload))
            graph.inject("source", Message(4, 10))
            graph.run_until_idle()
            self.assertEqual(helpers.payloads(graph.poll("primary")), [8])
            self.assertEqual(helpers.payloads(graph.poll("copy")), [8])
        # Each output got its own sink, and the tap still saw the message once.
        self.assertEqual(seen, [8])


if __name__ == "__main__":
    unittest.main()
