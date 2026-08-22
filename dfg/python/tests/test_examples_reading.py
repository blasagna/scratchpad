"""The stdlib examples, driven in process rather than by scraping their output."""

import io
import unittest
from contextlib import redirect_stdout

from dfg.blueprint import GraphBuilder, GraphSpec
from dfg.errors import ParamError
from dfg.graph import Graph, replay
from dfg.mermaid import render_mermaid
from dfg.message import Message
from dfg.readiness import AllInputs, AnyInput
from dfg.registry import Registry
from dfg.serialize import dumps, loads
from dfg.validate import check
from examples import (
    blueprint_roundtrip,
    lifecycle_demo,
    reading_pipeline,
    render_mermaid as render_demo,
    replay_demo,
    scheduling_demo,
)
from examples.nodes import core, reading
from examples.nodes.reading import Level
from examples.synth import reading_recording, synth_readings


def run_main(module) -> str:
    """Run a demo's ``main()`` and capture what it printed."""
    buffer = io.StringIO()
    with redirect_stdout(buffer):
        module.main()
    return buffer.getvalue()


class TestSynth(unittest.TestCase):
    def test_synth_is_deterministic(self):
        self.assertEqual(synth_readings(8), synth_readings(8))

    def test_timestamps_are_exact_at_200_hz(self):
        readings = synth_readings(4, rate_hz=200.0)
        self.assertEqual(
            [m.timestamp for m in readings], [0, 5_000_000, 10_000_000, 15_000_000]
        )

    def test_values_span_the_level_bands(self):
        # After the pipeline's 2x scale, values must reach every band: below the
        # active threshold (1.0), into MEDIUM, and past HIGH (2.0).
        scaled = [2.0 * m.payload.value for m in synth_readings(24)]
        self.assertTrue(any(v < 1.0 for v in scaled))
        self.assertTrue(any(1.0 <= v < 2.0 for v in scaled))
        self.assertTrue(any(v >= 2.0 for v in scaled))

    def test_a_different_seed_gives_different_samples(self):
        self.assertNotEqual(
            synth_readings(8, seed=1, noise=0.01),
            synth_readings(8, seed=2, noise=0.01),
        )


class TestCoreNodes(unittest.TestCase):
    def graph_of(self, type_name, params):
        builder = GraphBuilder("g")
        builder.add("n", type_name, params=params)
        builder.add_input("source", "n.inp")
        builder.add_output("output", "n.output")
        return Graph.instantiate(builder.build(), core.register(Registry()))

    def test_decimate_keeps_one_in_three(self):
        with self.graph_of(core.Decimate, {"factor": 3, "phase": 0}) as graph:
            for i in range(7):
                graph.inject("source", Message(i, i))
            graph.run_until_idle()
            self.assertEqual([m.payload for m in graph.poll("output")], [0, 3, 6])

    def test_decimate_phase_shifts_which_one_survives(self):
        with self.graph_of(core.Decimate, {"factor": 3, "phase": 2}) as graph:
            for i in range(7):
                graph.inject("source", Message(i, i))
            graph.run_until_idle()
            self.assertEqual([m.payload for m in graph.poll("output")], [2, 5])

    def test_decimate_rejects_a_factor_below_one(self):
        with self.assertRaises(ParamError):
            core.Decimate(factor=0)

    def test_window_count_matches_the_framing_formula(self):
        for n, size, hop in ((10, 4, 2), (10, 4, 4), (10, 3, 1), (9, 4, 5)):
            with self.subTest(n=n, size=size, hop=hop):
                with self.graph_of(core.Window, {"size": size, "hop": hop}) as graph:
                    for i in range(n):
                        graph.inject("source", Message(i, i))
                    graph.run_until_idle()
                    windows = graph.poll("output")
                self.assertEqual(len(windows), 1 + (n - size) // hop)
                self.assertTrue(all(len(w.payload) == size for w in windows))

    def test_a_window_carries_its_first_samples_time(self):
        with self.graph_of(core.Window, {"size": 3, "hop": 2}) as graph:
            for i in range(7):
                graph.inject("source", Message(i, i * 1000))
            graph.run_until_idle()
            windows = graph.poll("output")
        self.assertEqual([w.timestamp for w in windows], [0, 2000, 4000])
        self.assertEqual([w.payload[0].payload for w in windows], [0, 2, 4])

    def test_overlapping_windows_share_samples(self):
        with self.graph_of(core.Window, {"size": 4, "hop": 2}) as graph:
            for i in range(8):
                graph.inject("source", Message(i, i))
            graph.run_until_idle()
            windows = graph.poll("output")
        self.assertEqual(
            [[inner.payload for inner in w.payload] for w in windows],
            [[0, 1, 2, 3], [2, 3, 4, 5], [4, 5, 6, 7]],
        )

    def test_resample_holds_the_newest_fast_value(self):
        builder = GraphBuilder("g")
        # `any`, not the default `all`: the fast stream has to run ahead.
        builder.add("hold", core.Resample, readiness=AnyInput())
        builder.add_input("slow", "hold.slow")
        builder.add_input("fast", "hold.fast")
        builder.add_output("output", "hold.output")
        with Graph.instantiate(builder.build(), core.register(Registry())) as graph:
            # Nothing held yet, so nothing comes out.
            graph.inject("slow", Message("frame0", 0))
            graph.run_until_idle()
            self.assertEqual(graph.poll("output"), ())
            for i in range(3):
                graph.inject("fast", Message(f"fast{i}", i))
                graph.run_until_idle()
            graph.inject("slow", Message("frame1", 10))
            graph.run_until_idle()
            self.assertEqual(
                [m.payload for m in graph.poll("output")], [("frame1", "fast2")]
            )


class TestReadingPipeline(unittest.TestCase):
    def test_the_blueprint_validates(self):
        self.assertEqual(
            check(
                reading_pipeline.build_blueprint(), reading_pipeline.build_registry()
            ),
            (),
        )

    def test_the_readme_topics_and_aliases_appear(self):
        graph = Graph.instantiate(
            reading_pipeline.build_blueprint(), reading_pipeline.build_registry()
        )
        self.assertEqual(
            graph.topics,
            (
                "classify.flag.flagged",
                "classify.grade.graded",
                "relabel.labeled",
                "scale.scaled",
            ),
        )
        self.assertEqual(graph.aliases, ("classify.classified", "result"))
        self.assertEqual(
            graph.flat.aliases["classify.classified"], ("classify.grade", "graded")
        )

    def test_the_graph_parameter_reaches_the_nested_node(self):
        graph = Graph.instantiate(
            reading_pipeline.build_blueprint(), reading_pipeline.build_registry()
        )
        self.assertEqual(graph.flat.nodes["classify.flag"].params["threshold"], 1.0)

    def test_scale_doubles_the_value(self):
        outputs = []
        with Graph.instantiate(
            reading_pipeline.build_blueprint(), reading_pipeline.build_registry()
        ) as graph:
            graph.subscribe("scale.scaled", lambda name, m: outputs.append(m))
            for message in synth_readings(40, noise=0.0):
                graph.inject("readings", message)
                graph.run_until_idle()
        # scale is gain=2, offset=0, so the value is exactly doubled.
        self.assertAlmostEqual(outputs[1].payload.value, 0.2, places=9)

    def test_readings_get_flagged_graded_and_labelled(self):
        outputs = replay(
            reading_pipeline.build_blueprint(),
            reading_pipeline.build_registry(),
            reading_recording(40),
        )
        self.assertEqual(len(outputs["result"]), 40)
        last = outputs["result"][-1].payload
        # relabel folds the tag and the level name into the label.
        self.assertIn(":", last.label)
        self.assertIn(last.level.name, last.label)
        self.assertIsInstance(last.level, Level)

    def test_the_alias_and_the_topic_see_identical_messages(self):
        via_alias, via_topic = [], []
        with Graph.instantiate(
            reading_pipeline.build_blueprint(), reading_pipeline.build_registry()
        ) as graph:
            graph.subscribe("classify.classified", lambda name, m: via_alias.append(m))
            graph.subscribe(
                "classify.grade.graded", lambda name, m: via_topic.append(m)
            )
            for name, message in reading_recording(8):
                graph.inject(name, message)
                graph.run_until_idle()
        self.assertEqual(len(via_alias), 8)
        self.assertEqual(via_alias, via_topic)

    def test_rerunning_gives_identical_output(self):
        recording = reading_recording(16)
        first = replay(
            reading_pipeline.build_blueprint(),
            reading_pipeline.build_registry(),
            recording,
        )
        second = replay(
            reading_pipeline.build_blueprint(),
            reading_pipeline.build_registry(),
            recording,
        )
        self.assertEqual(
            [(m.payload, m.timestamp) for m in first["result"]],
            [(m.payload, m.timestamp) for m in second["result"]],
        )

    def test_the_subgraph_is_reusable_at_another_threshold(self):
        # Which is the whole point of the $param reference.
        spec = reading_pipeline.build_blueprint()
        retuned = GraphSpec(
            name=spec.name,
            nodes=spec.nodes,
            edges=spec.edges,
            inputs=spec.inputs,
            outputs=spec.outputs,
            params={"active_threshold": 1.5},
        )
        graph = Graph.instantiate(retuned, reading_pipeline.build_registry())
        self.assertEqual(graph.flat.nodes["classify.flag"].params["threshold"], 1.5)


class TestLifecycleDemo(unittest.TestCase):
    def test_the_clean_run_traces_setup_then_reverse_teardown(self):
        trace, produced = lifecycle_demo.run_clean()
        self.assertEqual(
            lifecycle_demo.phases_of(trace, "setup"), ["first", "middle", "last"]
        )
        self.assertEqual(
            lifecycle_demo.phases_of(trace, "teardown"), ["last", "middle", "first"]
        )
        self.assertEqual(len(produced), 2)

    def test_the_failing_run_shows_who_gets_a_teardown(self):
        trace, error = lifecycle_demo.run_failing()
        self.assertEqual(lifecycle_demo.phases_of(trace, "setup"), ["first", "middle"])
        self.assertEqual(lifecycle_demo.phases_of(trace, "teardown"), ["first"])
        self.assertIn("middle", error)

    def test_main_runs(self):
        self.assertIn("teardown order", run_main(lifecycle_demo))


class TestSchedulingDemo(unittest.TestCase):
    def test_the_orderings_disagree_about_firing_order(self):
        orders = {}
        for ordering, priority in (("topological", 0), ("level", 0), ("priority", 10)):
            trace: list[tuple[str, str]] = []
            scheduling_demo.run(
                scheduling_demo.ordering_blueprint(trace, side_priority=priority),
                ordering,
            )
            orders[ordering] = [name for name, phase in trace if phase == "run"][:5]
        self.assertEqual(
            orders["topological"], ["head", "chain1", "chain2", "chain3", "side"]
        )
        self.assertEqual(
            orders["level"], ["head", "chain1", "side", "chain2", "chain3"]
        )
        self.assertEqual(
            orders["priority"], ["head", "side", "chain1", "chain2", "chain3"]
        )

    def test_the_orderings_agree_about_output(self):
        digests = set()
        for ordering in ("topological", "level", "priority"):
            result = scheduling_demo.run(
                scheduling_demo.ordering_blueprint([]), ordering
            )
            digests.add(tuple(result["digest"]))
        self.assertEqual(len(digests), 1)

    def test_readiness_changes_what_a_firing_means(self):
        strict = scheduling_demo.run(scheduling_demo.readiness_blueprint(AllInputs()))
        loose = scheduling_demo.run(scheduling_demo.readiness_blueprint(AnyInput()))
        self.assertLess(strict["join_fired"], loose["join_fired"])
        self.assertGreater(strict["left_pending"], 0)
        self.assertEqual(loose["left_pending"], 0)

    def test_main_runs(self):
        self.assertIn("readiness", run_main(scheduling_demo))


class TestRoundtripDemo(unittest.TestCase):
    def test_the_blueprint_round_trips_exactly(self):
        spec = reading_pipeline.build_blueprint()
        text = dumps(spec)
        self.assertEqual(loads(text), spec)
        self.assertEqual(dumps(loads(text)), text)

    def test_main_runs(self):
        output = run_main(blueprint_roundtrip)
        self.assertIn("reloaded == original:        True", output)
        self.assertIn("identical:     True", output)


class TestMermaidDemo(unittest.TestCase):
    def test_the_diagram_names_the_subgraph_and_its_topics(self):
        diagram = render_mermaid(reading_pipeline.build_blueprint())
        self.assertIn("subgraph classify[classify]", diagram)
        self.assertIn('"classify.flag.flagged"', diagram)
        self.assertIn("([readings])", diagram)

    def test_main_runs(self):
        self.assertIn("```mermaid", run_main(render_demo))


class TestReplayDemo(unittest.TestCase):
    def test_every_variant_hashes_the_same(self):
        spec = reading_pipeline.build_blueprint()
        recording = reading_recording(12)
        baseline = replay_demo.digest(
            replay(spec, reading_pipeline.build_registry(), recording)
        )
        for seed in range(4):
            variant = replay_demo.shuffled(spec, seed=seed)
            self.assertEqual(
                replay_demo.digest(
                    replay(variant, reading_pipeline.build_registry(), recording)
                ),
                baseline,
            )

    def test_the_digest_covers_timestamps(self):
        one = {"output": (Message("a", 1),)}
        two = {"output": (Message("a", 2),)}
        self.assertNotEqual(replay_demo.digest(one), replay_demo.digest(two))

    def test_main_runs(self):
        self.assertIn("distinct digests: 1", run_main(replay_demo))


if __name__ == "__main__":
    unittest.main()
