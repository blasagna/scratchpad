"""Transports, capacity, and the three overflow policies (there is no fourth)."""

import unittest

from dfg.blueprint import Overflow
from dfg.errors import EdgeOverflowError, UnknownTransportError
from dfg.message import Envelope, Message
from dfg.transport import (
    EdgeConfig,
    InMemoryTransport,
    PortQueueView,
    Transport,
    is_registered_transport,
    make_transport,
    register_transport,
    transport_names,
)


def envelope(payload, enqueued_ns=0):
    return Envelope(Message(payload, timestamp=payload * 10), enqueued_ns)


class TestUnbounded(unittest.TestCase):
    def test_unbounded_is_the_default(self):
        transport = InMemoryTransport(EdgeConfig("a -> b"))
        self.assertIsNone(transport.config.capacity)
        for i in range(1000):
            transport.put(envelope(i))
        self.assertEqual(len(transport), 1000)
        self.assertEqual(transport.dropped, 0)

    def test_fifo_order(self):
        transport = InMemoryTransport(EdgeConfig("a -> b"))
        for i in range(3):
            transport.put(envelope(i))
        self.assertEqual([transport.get().message.payload for _ in range(3)], [0, 1, 2])

    def test_peek_does_not_consume(self):
        transport = InMemoryTransport(EdgeConfig("a -> b"))
        transport.put(envelope(7))
        transport.put(envelope(8))
        self.assertEqual(transport.peek().message.payload, 7)
        self.assertEqual(transport.peek(1).message.payload, 8)
        self.assertEqual(len(transport), 2)

    def test_get_on_empty_raises(self):
        with self.assertRaises(IndexError):
            InMemoryTransport(EdgeConfig("a -> b")).get()

    def test_clear_reports_what_it_discarded_and_is_not_a_drop(self):
        transport = InMemoryTransport(EdgeConfig("a -> b"))
        for i in range(4):
            transport.put(envelope(i))
        self.assertEqual(transport.clear(), 4)
        self.assertEqual(len(transport), 0)
        self.assertEqual(transport.dropped, 0)

    def test_satisfies_the_transport_protocol(self):
        self.assertIsInstance(InMemoryTransport(EdgeConfig("a -> b")), Transport)


class TestOverflowPolicies(unittest.TestCase):
    def test_error_raises_and_names_the_edge(self):
        transport = InMemoryTransport(
            EdgeConfig("head.output -> tail.inp", capacity=2, on_overflow="error")
        )
        transport.put(envelope(0))
        transport.put(envelope(1))
        with self.assertRaises(EdgeOverflowError) as caught:
            transport.put(envelope(2))
        self.assertIn("head.output -> tail.inp", str(caught.exception))
        self.assertIn("drop_oldest/drop_newest", str(caught.exception))
        self.assertEqual(len(transport), 2)

    def test_drop_oldest_keeps_the_newest(self):
        transport = InMemoryTransport(
            EdgeConfig("a -> b", capacity=2, on_overflow="drop_oldest")
        )
        for i in range(4):
            transport.put(envelope(i))
        self.assertEqual([transport.get().message.payload for _ in range(2)], [2, 3])
        self.assertEqual(transport.dropped, 2)

    def test_drop_newest_keeps_the_oldest(self):
        transport = InMemoryTransport(
            EdgeConfig("a -> b", capacity=2, on_overflow="drop_newest")
        )
        for i in range(4):
            transport.put(envelope(i))
        self.assertEqual([transport.get().message.payload for _ in range(2)], [0, 1])
        self.assertEqual(transport.dropped, 2)

    def test_a_capacity_of_one_still_works(self):
        transport = InMemoryTransport(
            EdgeConfig("a -> b", capacity=1, on_overflow="drop_oldest")
        )
        transport.put(envelope(1))
        transport.put(envelope(2))
        self.assertEqual(len(transport), 1)
        self.assertEqual(transport.get().message.payload, 2)

    def test_block_is_not_a_policy(self):
        # It deadlocks a single-threaded scheduler instantly: the producer waits
        # for space only the consumer can free, and the consumer only runs when the
        # producer returns.
        self.assertNotIn("block", Overflow)
        self.assertEqual(
            sorted(p.value for p in Overflow),
            ["drop_newest", "drop_oldest", "error"],
        )

    def test_the_queue_has_no_maxlen_so_drops_are_counted(self):
        # A deque with maxlen would silently drop the oldest, making the `error`
        # policy unimplementable and hiding drops from the counter.
        transport = InMemoryTransport(EdgeConfig("a -> b", capacity=1))
        self.assertIsNone(transport._queue.maxlen)


class TestPortQueueView(unittest.TestCase):
    def setUp(self):
        self.transport = InMemoryTransport(EdgeConfig("a -> b"))
        self.dequeued: list[Envelope] = []
        self.view = PortQueueView(
            self.transport, lambda t, env: self.dequeued.append(env)
        )

    def test_length_and_peek_see_messages_not_envelopes(self):
        self.transport.put(envelope(3))
        self.assertEqual(len(self.view), 1)
        self.assertIsInstance(self.view.peek(), Message)
        self.assertEqual(self.view.peek().payload, 3)

    def test_take_unwraps_and_reports_each_dequeue(self):
        for i in range(3):
            self.transport.put(envelope(i))
        taken = self.view.take(2)
        self.assertEqual([m.payload for m in taken], [0, 1])
        self.assertEqual(len(self.dequeued), 2)
        self.assertEqual(len(self.view), 1)

    def test_take_more_than_available_returns_what_there_is(self):
        self.transport.put(envelope(1))
        self.assertEqual(len(self.view.take(10)), 1)

    def test_take_from_empty_is_empty(self):
        self.assertEqual(self.view.take(1), ())

    def test_an_envelope_never_reaches_the_caller(self):
        # Which is what keeps sample time and wall-clock latency from being
        # conflated: only the control plane ever sees enqueued_ns.
        self.transport.put(envelope(1, enqueued_ns=999))
        (message,) = self.view.take(1)
        self.assertFalse(hasattr(message, "enqueued_ns"))
        self.assertEqual(self.dequeued[0].enqueued_ns, 999)


class TestTransportRegistry(unittest.TestCase):
    def test_memory_is_registered(self):
        self.assertIn("memory", transport_names())
        self.assertTrue(is_registered_transport("memory"))
        self.assertIsInstance(
            make_transport("memory", EdgeConfig("a -> b")), InMemoryTransport
        )

    def test_an_unknown_transport_names_the_edge_and_the_alternatives(self):
        with self.assertRaises(UnknownTransportError) as caught:
            make_transport("carrier_pigeon", EdgeConfig("a -> b"))
        self.assertIn("a -> b", str(caught.exception))
        self.assertIn("memory", str(caught.exception))

    def test_a_new_transport_is_a_registration_not_an_api_change(self):
        # Requirement 12: the transport is an edge property, and adding one does
        # not touch the node API.
        name = "test_only_double_buffer"
        register_transport(name, InMemoryTransport)
        try:
            self.assertTrue(is_registered_transport(name))
            with self.assertRaises(ValueError):
                register_transport(name, InMemoryTransport)
        finally:
            from dfg import transport as transport_module

            del transport_module._TRANSPORTS[name]


if __name__ == "__main__":
    unittest.main()
