"""Determinism: the same blueprint plus the same injected sequence, twice.

This is what makes offline reprocessing of a recording trustworthy, and it is the
constraint every scheduler added later inherits. The tests below attack it from
three directions: rerunning, reordering the declaration, and changing the ordering
policy.
"""

import random
import unittest

import helpers
from dfg.blueprint import GraphSpec
from dfg.graph import Graph, replay
from dfg.message import Message, ts_from_sample_index


def recording(count: int = 12, *, seed: int = 20260808):
    """A deterministic ``(input name, message)`` sequence at 200 Hz."""
    rng = random.Random(seed)
    return [
        (
            "source",
            Message(round(rng.uniform(-1.0, 1.0), 6), ts_from_sample_index(i, 200.0)),
        )
        for i in range(count)
    ]


def digest_outputs(outputs):
    """Payloads *and* timestamps, so a run that lost the sample times fails."""
    return {name: helpers.digest(messages) for name, messages in outputs.items()}


def shuffled(spec: GraphSpec, *, seed: int) -> GraphSpec:
    """The same blueprint with its node and edge declaration order shuffled."""
    rng = random.Random(seed)
    nodes = list(spec.nodes)
    edges = list(spec.edges)
    rng.shuffle(nodes)
    rng.shuffle(edges)
    return GraphSpec(
        name=spec.name,
        nodes=tuple(nodes),
        edges=tuple(edges),
        inputs=spec.inputs,
        outputs=spec.outputs,
        params=spec.params,
    )


class TestReplay(unittest.TestCase):
    def test_two_fresh_instances_produce_identical_output(self):
        spec = helpers.scheduling_spec()
        first = replay(spec, helpers.build_registry(), recording())
        second = replay(spec, helpers.build_registry(), recording())
        self.assertEqual(digest_outputs(first), digest_outputs(second))
        self.assertTrue(any(first.values()))

    def test_timestamps_are_compared_too(self):
        # A run that dropped the sample times would still match on payloads, so the
        # digest carries both.
        spec = helpers.scheduling_spec()
        outputs = replay(spec, helpers.build_registry(), recording())
        stamps = [ts for _, ts in digest_outputs(outputs)["chained"]]
        self.assertEqual(stamps, [ts_from_sample_index(i, 200.0) for i in range(12)])

    def test_the_firing_trace_is_identical_too(self):
        traces = []
        for _ in range(2):
            trace: list[tuple[str, str]] = []
            spec = helpers.scheduling_spec(trace)
            with Graph.instantiate(spec, helpers.build_registry()) as graph:
                for name, message in recording():
                    graph.inject(name, message)
                    graph.run_until_idle()
            traces.append(trace)
        self.assertEqual(traces[0], traces[1])

    def test_injecting_everything_up_front_is_also_deterministic(self):
        spec = helpers.scheduling_spec()
        results = []
        for _ in range(2):
            with Graph.instantiate(spec, helpers.build_registry()) as graph:
                for name, message in recording():
                    graph.inject(name, message)
                graph.run_until_idle()
                results.append(digest_outputs(graph.poll_all()))
        self.assertEqual(results[0], results[1])

    def test_a_wall_clock_difference_does_not_change_the_output(self):
        # Latency measurement reads a clock; nothing that affects output does.
        spec = helpers.scheduling_spec()
        fast = replay(
            spec, helpers.build_registry(), recording(), clock=helpers.FakeClock(step=1)
        )
        slow = replay(
            spec,
            helpers.build_registry(),
            recording(),
            clock=helpers.FakeClock(start=10**15, step=10**6),
        )
        self.assertEqual(digest_outputs(fast), digest_outputs(slow))


class TestDeclarationOrderIndependence(unittest.TestCase):
    """What tie-breaking by qualified ID actually buys."""

    def test_shuffling_the_declaration_order_changes_nothing(self):
        spec = helpers.scheduling_spec()
        baseline = digest_outputs(replay(spec, helpers.build_registry(), recording()))
        for seed in range(5):
            with self.subTest(seed=seed):
                variant = shuffled(spec, seed=seed)
                self.assertEqual(
                    digest_outputs(
                        replay(variant, helpers.build_registry(), recording())
                    ),
                    baseline,
                )

    def test_shuffling_does_not_change_the_firing_order_either(self):
        def trace_of(build):
            trace: list[tuple[str, str]] = []
            with Graph.instantiate(
                build(helpers.scheduling_spec(trace)), helpers.build_registry()
            ) as graph:
                for name, message in recording(4):
                    graph.inject(name, message)
                    graph.run_until_idle()
            return [label for label, phase in trace if phase == "run"]

        baseline = trace_of(lambda spec: spec)
        for seed in range(3):
            with self.subTest(seed=seed):
                order = trace_of(lambda spec, seed=seed: shuffled(spec, seed=seed))
                self.assertEqual(order, baseline)

    def test_the_canonical_order_itself_is_declaration_independent(self):
        spec = helpers.scheduling_spec()
        baseline = Graph.instantiate(spec, helpers.build_registry()).order
        for seed in range(5):
            with self.subTest(seed=seed):
                variant = Graph.instantiate(
                    shuffled(spec, seed=seed), helpers.build_registry()
                )
                self.assertEqual(variant.order, baseline)


class TestOrderingIndependence(unittest.TestCase):
    def test_different_orderings_agree_on_outputs(self):
        # They disagree on firing order -- see test_scheduler -- and must still
        # agree on what came out. Otherwise swapping the scheduler is a behaviour
        # change and requirement 8 means much less than it says.
        spec = helpers.scheduling_spec()
        results = {
            name: digest_outputs(
                replay(spec, helpers.build_registry(), recording(), ordering=name)
            )
            for name in ("topological", "level", "priority")
        }
        self.assertEqual(results["topological"], results["level"])
        self.assertEqual(results["topological"], results["priority"])

    def test_each_ordering_is_stable_across_repeats(self):
        spec = helpers.scheduling_spec()
        for name in ("topological", "level", "priority"):
            with self.subTest(ordering=name):
                first = replay(
                    spec, helpers.build_registry(), recording(), ordering=name
                )
                second = replay(
                    spec, helpers.build_registry(), recording(), ordering=name
                )
                self.assertEqual(digest_outputs(first), digest_outputs(second))


class TestNestedGraphs(unittest.TestCase):
    def test_a_nested_blueprint_replays_identically(self):
        spec = helpers.readme_example_spec()
        pairs = [
            ("imu_raw", Message(f"imu{i}", ts_from_sample_index(i, 200.0)))
            for i in range(6)
        ]
        frames = [
            ("frames", Message(f"frame{i}", ts_from_sample_index(i, 200.0)))
            for i in range(6)
        ]
        interleaved = [pair for both in zip(pairs, frames) for pair in both]
        first = replay(spec, helpers.build_registry(), interleaved)
        second = replay(spec, helpers.build_registry(), interleaved)
        self.assertEqual(digest_outputs(first), digest_outputs(second))
        self.assertEqual(len(first["pose"]), 6)


class TestReplayHelper(unittest.TestCase):
    def test_replay_returns_every_declared_output(self):
        outputs = replay(
            helpers.scheduling_spec(), helpers.build_registry(), recording(3)
        )
        self.assertEqual(sorted(outputs), ["aside", "chained"])

    def test_replay_tears_the_graph_down(self):
        trace: list[tuple[str, str]] = []
        replay(
            helpers.chain_spec(labels=("a", "b"), trace=trace),
            helpers.build_registry(),
            recording(2),
        )
        self.assertEqual(
            [label for label, phase in trace if phase == "teardown"], ["b", "a"]
        )

    def test_an_empty_recording_produces_empty_outputs(self):
        outputs = replay(helpers.scheduling_spec(), helpers.build_registry(), [])
        self.assertEqual(outputs, {"chained": (), "aside": ()})


if __name__ == "__main__":
    unittest.main()
