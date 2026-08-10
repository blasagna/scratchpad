"""The audio example: numpy payloads, many outputs per firing, backpressure."""

import io
import unittest
from contextlib import redirect_stdout

from optional import HAVE_NUMPY, NUMPY_REASON

if HAVE_NUMPY:
    import numpy as np

    from dfg.blueprint import GraphBuilder
    from dfg.errors import ParamError
    from dfg.graph import Graph
    from dfg.message import Message
    from dfg.registry import Registry
    from dfg.validate import check
    from examples import audio_pipeline
    from examples.nodes import audio, core
    from examples.synth_media import (
        DEFAULT_SAMPLE_RATE,
        synth_noise,
        synth_tone,
    )


@unittest.skipUnless(HAVE_NUMPY, NUMPY_REASON)
class TestSynthMedia(unittest.TestCase):
    def test_blocks_are_float32_and_the_right_shape(self):
        blocks = synth_tone(3, block_size=128)
        self.assertEqual(len(blocks), 3)
        for message in blocks:
            self.assertEqual(message.payload.dtype, np.float32)
            self.assertEqual(message.payload.shape, (128,))

    def test_synthesis_is_deterministic(self):
        first = synth_tone(3, block_size=64)
        second = synth_tone(3, block_size=64)
        for a, b in zip(first, second):
            np.testing.assert_array_equal(a.payload, b.payload)
            self.assertEqual(a.timestamp, b.timestamp)

    def test_blocks_are_contiguous_in_time(self):
        blocks = synth_tone(4, block_size=256, sample_rate=16_000.0)
        step = blocks[1].timestamp - blocks[0].timestamp
        self.assertEqual(step, round(256 * 1e9 / 16_000.0))
        self.assertEqual(blocks[0].timestamp, 0)

    def test_mismatched_amplitudes_are_rejected(self):
        with self.assertRaises(ValueError):
            synth_tone(1, frequencies=(440.0, 880.0), amplitudes=(1.0,))


@unittest.skipUnless(HAVE_NUMPY, NUMPY_REASON)
class TestFraming(unittest.TestCase):
    def frame_graph(self, size, hop, block_size=512, sample_rate=16_000.0):
        builder = GraphBuilder("g")
        builder.add(
            "frame",
            audio.Frame,
            params={"size": size, "hop": hop, "sample_rate": sample_rate},
        )
        builder.add_input("blocks", "frame.input")
        builder.add_output("windows", "frame.output")
        registry = audio.register(core.register(Registry()))
        return Graph.instantiate(builder.build(), registry), block_size

    def test_window_count_matches_the_framing_formula(self):
        for blocks, block_size, size, hop in (
            (8, 512, 256, 128),
            (4, 256, 256, 256),
            (3, 100, 64, 16),
        ):
            with self.subTest(blocks=blocks, size=size, hop=hop):
                graph, _ = self.frame_graph(size, hop, block_size)
                with graph:
                    for message in synth_tone(blocks, block_size=block_size):
                        graph.inject("blocks", message)
                        graph.run_until_idle()
                    windows = graph.poll("windows")
                total = blocks * block_size
                self.assertEqual(len(windows), 1 + (total - size) // hop)
                for window in windows:
                    self.assertEqual(window.payload.shape, (size,))

    def test_one_firing_can_emit_several_windows(self):
        # The reason zero-or-more is the contract rather than a convenience.
        graph, _ = self.frame_graph(256, 128, block_size=512)
        with graph:
            graph.inject("blocks", synth_tone(1, block_size=512)[0])
            graph.run_until_idle()
            self.assertEqual(graph.control.node_stats()["frame"].fired, 1)
            self.assertGreater(len(graph.poll("windows")), 1)

    def test_a_window_carries_its_own_first_samples_time(self):
        graph, _ = self.frame_graph(256, 128, block_size=512, sample_rate=16_000.0)
        with graph:
            for message in synth_tone(2, block_size=512, sample_rate=16_000.0):
                graph.inject("blocks", message)
                graph.run_until_idle()
            windows = graph.poll("windows")
        expected = [round(i * 128 * 1e9 / 16_000.0) for i in range(len(windows))]
        self.assertEqual([w.timestamp for w in windows], expected)

    def test_overlapping_windows_share_samples(self):
        graph, _ = self.frame_graph(4, 2, block_size=8)
        with graph:
            graph.inject("blocks", Message(np.arange(8, dtype=np.float32), 0))
            graph.run_until_idle()
            windows = graph.poll("windows")
        self.assertEqual(
            [w.payload.tolist() for w in windows],
            [[0, 1, 2, 3], [2, 3, 4, 5], [4, 5, 6, 7]],
        )

    def test_a_hop_larger_than_the_size_is_rejected(self):
        # That would silently discard samples, which is decimation and belongs in
        # its own node.
        with self.assertRaises(ParamError):
            audio.Frame(size=64, hop=128)

    def test_framing_works_across_a_block_boundary(self):
        # A 6-sample window never lines up with a 4-sample block, so every window
        # after the first is stitched from two blocks.
        graph, _ = self.frame_graph(6, 6, block_size=4)
        with graph:
            for i in range(3):
                graph.inject(
                    "blocks", Message(np.arange(i * 4, i * 4 + 4, dtype=np.float32), i)
                )
                graph.run_until_idle()
            windows = graph.poll("windows")
        self.assertEqual(
            [w.payload.tolist() for w in windows],
            [[0, 1, 2, 3, 4, 5], [6, 7, 8, 9, 10, 11]],
        )


@unittest.skipUnless(HAVE_NUMPY, NUMPY_REASON)
class TestAnalysisNodes(unittest.TestCase):
    def test_hann_tapers_the_ends_and_keeps_the_middle(self):
        node = audio.Hann()
        node.setup()
        ones = Message(np.ones(8, dtype=np.float32), 0)
        (out,) = node.run({"input": (ones,)})["output"]
        self.assertAlmostEqual(float(out.payload[0]), 0.0, places=6)
        self.assertGreater(float(out.payload[4]), 0.9)
        self.assertEqual(out.payload.dtype, np.float32)

    def test_rms_of_a_unit_sine_is_about_minus_three_db(self):
        node = audio.Rms()
        samples = np.sin(np.linspace(0, 2 * np.pi, 1024, endpoint=False))
        (out,) = node.run({"input": (Message(samples.astype(np.float32), 0),)})[
            "output"
        ]
        self.assertAlmostEqual(out.payload, -3.01, places=1)

    def test_rms_of_silence_hits_the_floor(self):
        node = audio.Rms(floor_db=-120.0)
        (out,) = node.run({"input": (Message(np.zeros(16, dtype=np.float32), 0),)})[
            "output"
        ]
        self.assertEqual(out.payload, -120.0)

    def test_spectrum_length_is_the_rfft_length(self):
        node = audio.Spectrum()
        (out,) = node.run({"input": (Message(np.zeros(256, dtype=np.float32), 0),)})[
            "output"
        ]
        self.assertEqual(out.payload.shape, (129,))

    def test_peak_bin_finds_an_injected_tone(self):
        size, rate, frequency = 256, 16_000.0, 1_000.0
        t = np.arange(size) / rate
        samples = np.sin(2 * np.pi * frequency * t).astype(np.float32)
        spectrum = audio.Spectrum()
        peak = audio.PeakBin(sample_rate=rate, window_size=size)
        (magnitudes,) = spectrum.run({"input": (Message(samples, 0),)})["output"]
        (out,) = peak.run({"input": (magnitudes,)})["output"]
        self.assertAlmostEqual(out.payload, frequency, delta=rate / size)

    def test_peak_bin_ignores_dc(self):
        # A signal with an offset would otherwise always peak at bin 0.
        size, rate = 256, 16_000.0
        t = np.arange(size) / rate
        samples = (5.0 + np.sin(2 * np.pi * 1_000.0 * t)).astype(np.float32)
        spectrum = audio.Spectrum()
        peak = audio.PeakBin(sample_rate=rate, window_size=size)
        (magnitudes,) = spectrum.run({"input": (Message(samples, 0),)})["output"]
        (out,) = peak.run({"input": (magnitudes,)})["output"]
        self.assertGreater(out.payload, 500.0)


@unittest.skipUnless(HAVE_NUMPY, NUMPY_REASON)
class TestAudioPipeline(unittest.TestCase):
    def test_the_blueprint_validates(self):
        self.assertEqual(
            check(audio_pipeline.build_blueprint(), audio_pipeline.build_registry()), ()
        )

    def test_the_detected_peak_matches_the_injected_tone(self):
        blocks = synth_tone(
            8,
            block_size=audio_pipeline.BLOCK_SIZE,
            sample_rate=DEFAULT_SAMPLE_RATE,
            frequencies=(1_000.0,),
            noise=0.0,
        )
        summaries, _, _ = audio_pipeline.run(blocks)
        peaks = {peak for _, peak in (m.payload for m in summaries)}
        self.assertEqual(peaks, {1_000.0})

    def test_bin_resolution_limits_the_lower_tone(self):
        # 440 Hz cannot be reported exactly by a 256-point window at 16 kHz: the
        # bins are 62.5 Hz apart, so the answer is 437.5. Not a bug -- arithmetic.
        blocks = synth_tone(
            4,
            block_size=audio_pipeline.BLOCK_SIZE,
            sample_rate=DEFAULT_SAMPLE_RATE,
            frequencies=(440.0,),
            noise=0.0,
        )
        summaries, _, _ = audio_pipeline.run(blocks)
        peaks = {peak for _, peak in (m.payload for m in summaries)}
        self.assertEqual(peaks, {437.5})

    def test_window_count_matches_the_formula(self):
        blocks = synth_tone(8, block_size=audio_pipeline.BLOCK_SIZE)
        _, windows, _ = audio_pipeline.run(blocks)
        total = 8 * audio_pipeline.BLOCK_SIZE
        self.assertEqual(
            windows,
            1 + (total - audio_pipeline.WINDOW_SIZE) // audio_pipeline.HOP,
        )

    def test_dtype_and_shape_survive_the_graph(self):
        blocks = synth_tone(2, block_size=audio_pipeline.BLOCK_SIZE)
        windows = []
        spec = audio_pipeline.build_blueprint()
        with Graph.instantiate(spec, audio_pipeline.build_registry()) as graph:
            graph.subscribe("hann.output", lambda name, m: windows.append(m))
            for message in blocks:
                graph.inject("blocks", message)
                graph.run_until_idle()
        for window in windows:
            self.assertEqual(window.payload.dtype, np.float32)
            self.assertEqual(window.payload.shape, (audio_pipeline.WINDOW_SIZE,))

    def test_white_noise_has_no_single_peak(self):
        summaries, _, _ = audio_pipeline.run(
            synth_noise(8, block_size=audio_pipeline.BLOCK_SIZE)
        )
        peaks = {peak for _, peak in (m.payload for m in summaries)}
        self.assertGreater(len(peaks), 5)

    def test_a_bounded_edge_drops_only_when_the_producer_runs_ahead(self):
        blocks = synth_tone(8, block_size=audio_pipeline.BLOCK_SIZE)
        _, _, drained = audio_pipeline.run(
            blocks, capacity=4, on_overflow="drop_oldest", drain_each=True
        )
        self.assertEqual(drained["frame.output -> hann.input"].dropped, 0)

        _, _, running_ahead = audio_pipeline.run(
            blocks, capacity=4, on_overflow="drop_oldest", drain_each=False
        )
        self.assertGreater(running_ahead["frame.output -> hann.input"].dropped, 0)

    def test_an_error_policy_edge_raises_when_it_overflows(self):
        from dfg.errors import EdgeOverflowError

        blocks = synth_tone(8, block_size=audio_pipeline.BLOCK_SIZE)
        with self.assertRaises(EdgeOverflowError):
            audio_pipeline.run(
                blocks, capacity=2, on_overflow="error", drain_each=False
            )

    def test_rerunning_gives_identical_output(self):
        blocks = synth_tone(4, block_size=audio_pipeline.BLOCK_SIZE)
        first, _, _ = audio_pipeline.run(blocks)
        second, _, _ = audio_pipeline.run(blocks)
        self.assertEqual(
            [(m.payload, m.timestamp) for m in first],
            [(m.payload, m.timestamp) for m in second],
        )

    def test_main_runs(self):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            audio_pipeline.main()
        output = buffer.getvalue()
        self.assertIn("Output cardinality", output)
        self.assertIn("Backpressure", output)


if __name__ == "__main__":
    unittest.main()
