"""Messages, timestamps, and the sample-time/wall-clock separation."""

import doctest
import unittest

from dfg import message, ports
from dfg.message import (
    NANOS_PER_SECOND,
    Envelope,
    Message,
    ts_from_sample_index,
    ts_from_seconds,
    ts_to_seconds,
)


class TestMessage(unittest.TestCase):
    def test_is_frozen(self):
        msg = Message(payload=1, timestamp=0)
        with self.assertRaises(Exception):
            # The assignment being a type error is the point of the test.
            msg.payload = 2  # pyrefly: ignore[read-only]

    def test_uses_slots_so_a_typo_cannot_add_a_field(self):
        msg = Message(payload=1, timestamp=0)
        self.assertFalse(hasattr(msg, "__dict__"))
        # The exception type is not pinned: a frozen slotted generic dataclass
        # raises AttributeError on 3.13+ and TypeError on 3.12. What matters is
        # that a misspelled field cannot silently become one.
        with self.assertRaises(Exception):
            msg.timestmap = 5  # pyrefly: ignore[missing-attribute]
        self.assertFalse(hasattr(msg, "timestmap"))

    def test_equality_and_hashing_cover_both_fields(self):
        self.assertEqual(Message("a", 7), Message("a", 7))
        self.assertNotEqual(Message("a", 7), Message("a", 8))
        self.assertNotEqual(Message("a", 7), Message("b", 7))
        self.assertEqual(len({Message("a", 7), Message("a", 7)}), 1)

    def test_with_payload_preserves_the_sample_time(self):
        original = Message(payload=[1, 2, 3], timestamp=1_234_567_890)
        derived = original.with_payload("summary")
        self.assertEqual(derived.payload, "summary")
        self.assertEqual(derived.timestamp, original.timestamp)
        self.assertEqual(original.payload, [1, 2, 3])

    def test_payload_may_be_any_object(self):
        sentinel = object()
        self.assertIs(Message(sentinel, 0).payload, sentinel)

    def test_carries_no_wall_clock_field(self):
        # Latency must never be derivable from a message, so that no node author
        # can conflate sample time with time spent in the graph.
        self.assertEqual(
            [f for f in Message.__dataclass_fields__], ["payload", "timestamp"]
        )


class TestEnvelope(unittest.TestCase):
    def test_separates_enqueue_time_from_sample_time(self):
        msg = Message("x", timestamp=10**18)  # a sample time hours from the epoch
        env = Envelope(message=msg, enqueued_ns=42)
        self.assertEqual(env.message.timestamp, 10**18)
        self.assertEqual(env.enqueued_ns, 42)


class TestTimestampHelpers(unittest.TestCase):
    def test_seconds_round_trip(self):
        self.assertEqual(ts_from_seconds(1.5), 1_500_000_000)
        self.assertEqual(ts_to_seconds(1_500_000_000), 1.5)

    def test_sample_index_is_exact_at_200_hz(self):
        # 200 Hz is 5 ms, a whole number of nanoseconds, so a synthesized signal
        # stream has exact timestamps and a replay test can assert equality.
        self.assertEqual(ts_from_sample_index(0, 200.0), 0)
        self.assertEqual(ts_from_sample_index(1, 200.0), 5_000_000)
        self.assertEqual(ts_from_sample_index(3, 200.0), 15_000_000)
        self.assertEqual(ts_from_sample_index(200, 200.0), NANOS_PER_SECOND)

    def test_sample_index_rounds_for_awkward_rates(self):
        self.assertEqual(ts_from_sample_index(1, 30.0), 33_333_333)
        self.assertEqual(ts_from_sample_index(30, 30.0), NANOS_PER_SECOND)

    def test_sample_index_rejects_a_non_positive_rate(self):
        for rate in (0.0, -1.0):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                ts_from_sample_index(1, rate)


class TestDocstringExamples(unittest.TestCase):
    def test_doctests_pass(self):
        for module in (message, ports):
            with self.subTest(module=module.__name__):
                result = doctest.testmod(module, verbose=False)
                self.assertEqual(result.failed, 0)


if __name__ == "__main__":
    unittest.main()
