"""The control plane: stats, latency, live parameters, and flow control."""

import unittest

import helpers
from dfg.blueprint import GraphBuilder
from dfg.errors import ImmutableParamError, ParamError
from dfg.graph import Graph
from dfg.message import Message

HOUR_NS = 3_600_000_000_000


def watcher_spec(seen=None, changes=None):
    builder = GraphBuilder("g")
    builder.add(
        "gainer",
        helpers.ParamWatcher,
        params={"gain": 1, "seen": seen, "changes": changes},
    )
    builder.add_input("source", "gainer.inp")
    builder.add_output("output", "gainer.output")
    return builder.build()


class TestLatency(unittest.TestCase):
    """Latency is wall-clock time in the graph, never a message's sample time."""

    def test_latency_is_measured_against_the_control_clock(self):
        clock = helpers.FakeClock(start=0, step=0)
        spec = helpers.chain_spec(labels=("a",))
        with Graph.instantiate(spec, helpers.build_registry(), clock=clock) as graph:
            graph.inject("source", Message(1, 0))  # enqueued at tick 0
            clock.advance(500)
            graph.run_until_idle()  # dequeued at tick 500
            stats = graph.control.edge_stats()["input:source -> n_a.inp"]
            self.assertEqual(stats.latency_ns_last, 500)
            self.assertEqual(stats.latency_ns_max, 500)
            self.assertEqual(stats.latency_ns_total, 500)
            self.assertEqual(stats.latency_ns_mean, 500.0)

    def test_sample_timestamps_hours_apart_do_not_inflate_latency(self):
        # This is the exact shortcut the contract forbids: reading the message
        # timestamp reports garbage the moment a recording is replayed faster than
        # real time.
        clock = helpers.FakeClock(start=0, step=0)
        spec = helpers.chain_spec(labels=("a",))
        with Graph.instantiate(spec, helpers.build_registry(), clock=clock) as graph:
            for i in range(3):
                graph.inject("source", Message(i, i * HOUR_NS))
            clock.advance(7)
            graph.run_until_idle()
            stats = graph.control.edge_stats()["input:source -> n_a.inp"]
            self.assertEqual(stats.latency_ns_max, 7)
            self.assertLess(stats.latency_ns_total, 100)
            # The sample times are untouched, and still hours apart.
            self.assertEqual(
                [m.timestamp for m in graph.poll("sink")],
                [0, HOUR_NS, 2 * HOUR_NS],
            )

    def test_max_and_mean_track_separately(self):
        clock = helpers.FakeClock(start=0, step=0)
        spec = helpers.chain_spec(labels=("a",))
        with Graph.instantiate(spec, helpers.build_registry(), clock=clock) as graph:
            graph.inject("source", Message(1, 0))
            clock.advance(10)
            graph.run_until_idle()
            graph.inject("source", Message(2, 0))
            clock.advance(30)
            graph.run_until_idle()
            stats = graph.control.edge_stats()["input:source -> n_a.inp"]
            self.assertEqual(stats.latency_ns_last, 30)
            self.assertEqual(stats.latency_ns_max, 30)
            self.assertEqual(stats.latency_ns_mean, 20.0)

    def test_mean_is_zero_before_anything_is_dequeued(self):
        spec = helpers.chain_spec(labels=("a",))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            stats = graph.control.edge_stats()["input:source -> n_a.inp"]
            self.assertEqual(stats.latency_ns_mean, 0.0)


class TestCounters(unittest.TestCase):
    def test_edge_counters_track_enqueue_dequeue_and_depth(self):
        spec = helpers.chain_spec(labels=("a", "b"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            for i in range(3):
                graph.inject("source", Message(i, i))
            stats = graph.control.edge_stats()["input:source -> n_a.inp"]
            self.assertEqual((stats.enqueued, stats.dequeued, stats.depth), (3, 0, 3))
            graph.run_until_idle()
            stats = graph.control.edge_stats()["input:source -> n_a.inp"]
            self.assertEqual((stats.enqueued, stats.dequeued, stats.depth), (3, 3, 0))

    def test_node_counters_track_firings(self):
        spec = helpers.chain_spec(labels=("a", "b"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            for i in range(3):
                graph.inject("source", Message(i, i))
            graph.run_until_idle()
            stats = graph.control.node_stats()
            self.assertEqual(stats["n_a"].fired, 3)
            self.assertEqual(stats["n_b"].fired, 3)
            self.assertEqual(stats["n_a"].errors, 0)
            self.assertTrue(stats["n_a"].setup_done)

    def test_queue_depth_and_total_pending(self):
        spec = helpers.chain_spec(labels=("a", "b"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            graph.inject("source", Message(1, 1))
            self.assertEqual(graph.control.queue_depth("input:source -> n_a.inp"), 1)
            self.assertEqual(graph.control.total_pending(), 1)
            graph.run_until_idle()
            self.assertEqual(graph.control.queue_depth("input:source -> n_a.inp"), 0)
            # The output sink still holds the result until it is polled.
            self.assertEqual(graph.control.total_pending(), 1)
            graph.poll("sink")
            self.assertEqual(graph.control.total_pending(), 0)

    def test_an_unknown_edge_key_is_a_key_error(self):
        spec = helpers.chain_spec(labels=("a",))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            with self.assertRaises(KeyError):
                graph.control.queue_depth("nope")

    def test_edge_keys_include_the_synthetic_input_and_output_edges(self):
        # An injected message travels the same path as a produced one, which is
        # much of why a replay is trustworthy -- so it has an edge, with stats.
        spec = helpers.chain_spec(labels=("a", "b"))
        with Graph.instantiate(spec, helpers.build_registry()) as graph:
            self.assertEqual(
                graph.control.edge_keys(),
                (
                    "input:source -> n_a.inp",
                    "n_a.output -> n_b.inp",
                    "n_b.output -> output:sink",
                ),
            )

    def test_a_subgraph_boundary_has_no_queue_of_its_own(self):
        # Flattening's price: a parent edge into a two-target subgraph input is two
        # queues, so depth is per flattened edge and there is nothing to report as
        # "the subgraph's input depth".
        with Graph.instantiate(
            helpers.readme_example_spec(), helpers.build_registry()
        ) as graph:
            keys = graph.control.edge_keys()
            self.assertIn("scale.scaled -> classify.flag.reading", keys)
            self.assertIn("scale.scaled -> classify.grade.reading", keys)
            self.assertFalse(any(key.endswith("classify.reading") for key in keys))


class TestLiveParameters(unittest.TestCase):
    def test_an_immutable_parameter_is_refused_at_the_call(self):
        # Immediately, so the caller learns at the call rather than whenever the
        # scheduler next runs.
        with Graph.instantiate(
            helpers.chain_spec(labels=("a",)), helpers.build_registry()
        ) as graph:
            with self.assertRaises(ImmutableParamError) as caught:
                graph.control.set_params("n_a", {"label": "renamed"})
            self.assertIn("MUTABLE_PARAMS", str(caught.exception))

    def test_an_undeclared_parameter_is_refused(self):
        with Graph.instantiate(watcher_spec(), helpers.build_registry()) as graph:
            with self.assertRaises(ParamError):
                graph.control.set_params("gainer", {"nope": 1})

    def test_an_unknown_node_is_a_key_error(self):
        with Graph.instantiate(watcher_spec(), helpers.build_registry()) as graph:
            with self.assertRaises(KeyError):
                graph.control.set_params("ghost", {"gain": 2})

    def test_a_mutable_parameter_changes_between_firings(self):
        # Never during one: mutating parameters underneath a running `run` is the
        # natural-looking implementation and makes every node author responsible
        # for their own locking.
        seen: list[int] = []
        with Graph.instantiate(watcher_spec(seen=seen), helpers.build_registry()) as g:
            for i in range(3):
                g.inject("source", Message(1, i))
            g.step()
            self.assertEqual(seen, [1])
            g.control.set_params("gainer", {"gain": 10})
            self.assertTrue(g.control.has_pending_params())
            g.run_until_idle()
            self.assertFalse(g.control.has_pending_params())
        # No firing saw a half-applied change: every value is 1 or 10, in order.
        self.assertEqual(seen, [1, 10, 10])

    def test_the_change_hook_is_called_with_only_what_changed(self):
        changes: list[dict] = []
        with Graph.instantiate(
            watcher_spec(changes=changes), helpers.build_registry()
        ) as graph:
            graph.control.set_params("gainer", {"gain": 4})
            graph.inject("source", Message(2, 10))
            graph.run_until_idle()
            self.assertEqual(changes, [{"gain": 4}])
            self.assertEqual(graph.control.params("gainer")["gain"], 4)

    def test_a_queued_change_lands_before_the_next_firing(self):
        seen: list[int] = []
        with Graph.instantiate(watcher_spec(seen=seen), helpers.build_registry()) as g:
            g.control.set_params("gainer", {"gain": 3})
            g.inject("source", Message(1, 10))
            g.run_until_idle()
            self.assertEqual(seen, [3])

    def test_params_reports_the_current_values(self):
        with Graph.instantiate(watcher_spec(), helpers.build_registry()) as graph:
            self.assertEqual(graph.control.params("gainer")["gain"], 1)
            with self.assertRaises(KeyError):
                graph.control.params("ghost")


class TestFlowControl(unittest.TestCase):
    def test_pause_stops_firing_and_resume_restores_it(self):
        with Graph.instantiate(
            helpers.chain_spec(labels=("a", "b")), helpers.build_registry()
        ) as graph:
            graph.inject("source", Message(1, 10))
            graph.control.pause()
            self.assertTrue(graph.control.paused)
            self.assertFalse(graph.step())
            self.assertEqual(graph.run_until_idle(), 0)
            # Injection still works while paused; the messages just queue up.
            graph.inject("source", Message(2, 20))
            self.assertEqual(graph.control.total_pending(), 2)
            graph.control.resume()
            self.assertFalse(graph.control.paused)
            self.assertEqual(graph.run_until_idle(), 4)
            self.assertEqual(helpers.payloads(graph.poll("sink")), [1, 2])

    def test_reset_pending_empties_every_queue_including_output_sinks(self):
        with Graph.instantiate(
            helpers.chain_spec(labels=("a", "b")), helpers.build_registry()
        ) as graph:
            graph.inject("source", Message(1, 10))
            graph.run_until_idle()
            graph.inject("source", Message(2, 20))
            self.assertEqual(graph.control.total_pending(), 2)  # sink + input
            self.assertEqual(graph.control.reset_pending(), 2)
            self.assertEqual(graph.control.total_pending(), 0)
            self.assertEqual(graph.poll("sink"), ())

    def test_reset_pending_is_not_counted_as_a_drop(self):
        # A drop is something a policy did to a graph that was trying to work; this
        # is the operator saying to start over.
        with Graph.instantiate(
            helpers.chain_spec(labels=("a",)), helpers.build_registry()
        ) as graph:
            graph.inject("source", Message(1, 10))
            graph.control.reset_pending()
            stats = graph.control.edge_stats()["input:source -> n_a.inp"]
            self.assertEqual(stats.dropped, 0)
            self.assertEqual(stats.depth, 0)


class TestClockInjection(unittest.TestCase):
    def test_the_clock_never_touches_message_timestamps(self):
        clock = helpers.FakeClock(start=10**12, step=7)
        spec = helpers.chain_spec(labels=("a",))
        with Graph.instantiate(spec, helpers.build_registry(), clock=clock) as graph:
            graph.inject("source", Message("payload", 42))
            graph.run_until_idle()
            (message,) = graph.poll("sink")
        self.assertEqual(message.timestamp, 42)

    def test_now_reads_the_injected_clock(self):
        clock = helpers.FakeClock(start=99, step=0)
        spec = helpers.chain_spec(labels=("a",))
        with Graph.instantiate(spec, helpers.build_registry(), clock=clock) as graph:
            self.assertEqual(graph.control.now(), 99)
            self.assertIs(graph.control.clock, clock)


if __name__ == "__main__":
    unittest.main()
