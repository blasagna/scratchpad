"""The video example: uint8 frame payloads, decimation, time alignment as a node."""

import io
import unittest
from contextlib import redirect_stdout

from optional import HAVE_NUMPY, NUMPY_REASON

if HAVE_NUMPY:
    import numpy as np

    from dfg.graph import Graph, replay
    from dfg.message import Message
    from dfg.validate import check
    from examples import video_pipeline
    from examples.nodes import video
    from examples.synth import synth_signal
    from examples.synth_media import frame_index_of, synth_frames


@unittest.skipUnless(HAVE_NUMPY, NUMPY_REASON)
class TestSynthFrames(unittest.TestCase):
    def test_frames_are_uint8_rgb_of_the_requested_size(self):
        frames = synth_frames(4, width=32, height=16)
        self.assertEqual(len(frames), 4)
        for message in frames:
            self.assertEqual(message.payload.dtype, np.uint8)
            self.assertEqual(message.payload.shape, (16, 32, 3))

    def test_a_frames_content_is_a_function_of_its_index(self):
        first = synth_frames(6)
        second = synth_frames(6)
        for a, b in zip(first, second):
            np.testing.assert_array_equal(a.payload, b.payload)

    def test_the_box_moves(self):
        frames = synth_frames(12)
        differing = np.any(frames[0].payload != frames[6].payload, axis=2)
        self.assertGreater(int(np.count_nonzero(differing)), 0)

    def test_the_bright_area_is_constant_because_the_box_only_moves(self):
        frames = synth_frames(10, box=8)
        for message in frames:
            bright = np.count_nonzero(np.all(message.payload > 100, axis=2))
            self.assertEqual(bright, 8 * 8)

    def test_timestamps_follow_the_frame_rate(self):
        frames = synth_frames(4, fps=30.0)
        self.assertEqual([frame_index_of(m.timestamp) for m in frames], [0, 1, 2, 3])


@unittest.skipUnless(HAVE_NUMPY, NUMPY_REASON)
class TestVideoNodes(unittest.TestCase):
    def test_to_gray_reduces_a_dimension_and_keeps_uint8(self):
        node = video.ToGray()
        frame = synth_frames(1, width=32, height=16)[0]
        (out,) = node.run(inp=(frame,)).output
        self.assertEqual(out.payload.shape, (16, 32))
        self.assertEqual(out.payload.dtype, np.uint8)
        self.assertEqual(out.timestamp, frame.timestamp)

    def test_to_gray_uses_the_rec_601_weights(self):
        node = video.ToGray()
        red = np.zeros((1, 1, 3), dtype=np.uint8)
        red[0, 0] = (255, 0, 0)
        (out,) = node.run(inp=(Message(red, 0),)).output
        self.assertEqual(int(out.payload[0, 0]), round(255 * 0.299))

    def test_overlay_changes_only_the_box_pixels(self):
        node = video.OverlayBox(size=6, colour=(0, 255, 0))
        frame = synth_frames(1, width=64, height=48)[0]
        (out,) = node.run(inp=(frame.with_payload((frame.payload, 0.0)),)).output

        changed = np.any(out.payload != frame.payload, axis=2)
        rows = np.flatnonzero(changed.any(axis=1))
        cols = np.flatnonzero(changed.any(axis=0))
        # A 6x6 box, and no more than that. Some of it may already have been green.
        self.assertLessEqual(len(rows), 6)
        self.assertLessEqual(len(cols), 6)
        self.assertLessEqual(int(np.count_nonzero(changed)), 36)

    def test_overlay_position_tracks_the_offset(self):
        node = video.OverlayBox(size=4, colour=(0, 255, 0), gain=40.0)
        frame = synth_frames(1, width=64, height=48)[0]

        def box_row(offset):
            (out,) = node.run(inp=(frame.with_payload((frame.payload, offset)),)).output
            green = np.all(out.payload == (0, 255, 0), axis=2)
            return int(np.flatnonzero(green.any(axis=1))[0])

        self.assertLess(box_row(-0.3), box_row(0.0))
        self.assertLess(box_row(0.0), box_row(0.3))

    def test_overlay_clamps_the_box_inside_the_frame(self):
        node = video.OverlayBox(size=6, gain=10_000.0)
        frame = synth_frames(1, width=64, height=48)[0]
        for offset in (-100.0, 100.0):
            (out,) = node.run(inp=(frame.with_payload((frame.payload, offset)),)).output
            self.assertEqual(out.payload.shape, frame.payload.shape)

    def test_frame_stats_summarizes_a_grey_frame(self):
        node = video.FrameStatsNode(bright_threshold=128)
        grey = np.zeros((4, 4), dtype=np.uint8)
        grey[0, :] = 200
        (out,) = node.run(inp=(Message(grey, 0),)).output
        self.assertEqual(out.payload.peak, 200)
        self.assertEqual(out.payload.bright_pixels, 4)
        self.assertAlmostEqual(out.payload.mean, 200 * 4 / 16, places=4)

    def test_downscale_halves_both_dimensions(self):
        node = video.Downscale(factor=2)
        frame = synth_frames(1, width=64, height=48)[0]
        (out,) = node.run(inp=(frame,)).output
        self.assertEqual(out.payload.shape, (24, 32, 3))
        self.assertEqual(out.payload.dtype, np.uint8)

    def test_downscale_refuses_an_inexact_factor(self):
        # Cropping or padding is a decision the caller should make on purpose.
        node = video.Downscale(factor=5)
        frame = synth_frames(1, width=64, height=48)[0]
        with self.assertRaises(ValueError):
            node.run(inp=(frame,))


@unittest.skipUnless(HAVE_NUMPY, NUMPY_REASON)
class TestVideoPipeline(unittest.TestCase):
    def setUp(self):
        self.frames = synth_frames(
            video_pipeline.FRAME_COUNT,
            width=video_pipeline.WIDTH,
            height=video_pipeline.HEIGHT,
            fps=video_pipeline.FPS,
        )
        duration = video_pipeline.FRAME_COUNT / video_pipeline.FPS
        self.samples = synth_signal(
            round(duration * video_pipeline.SAMPLE_RATE_HZ),
            rate_hz=video_pipeline.SAMPLE_RATE_HZ,
        )
        self.recording = video_pipeline.interleave(self.frames, self.samples)

    def test_the_blueprint_validates(self):
        self.assertEqual(
            check(video_pipeline.build_blueprint(), video_pipeline.build_registry()), ()
        )

    def test_the_recording_is_in_timestamp_order(self):
        stamps = [message.timestamp for _, message in self.recording]
        self.assertEqual(stamps, sorted(stamps))

    def test_decimation_keeps_one_frame_in_two(self):
        with Graph.instantiate(
            video_pipeline.build_blueprint(), video_pipeline.build_registry()
        ) as graph:
            for name, message in self.recording:
                graph.inject(name, message)
                graph.run_until_idle()
            kept = graph.control.edge_stats()["thin.output -> hold.slow"].enqueued
            fired = graph.control.node_stats()["thin"].fired
        self.assertEqual(fired, video_pipeline.FRAME_COUNT)
        self.assertEqual(kept, video_pipeline.FRAME_COUNT // video_pipeline.DECIMATE)

    def test_the_hold_pairs_each_frame_with_the_newest_track(self):
        tracks = []
        with Graph.instantiate(
            video_pipeline.build_blueprint(), video_pipeline.build_registry()
        ) as graph:
            graph.subscribe("integrate.track", lambda name, m: tracks.append(m))
            for name, message in self.recording:
                graph.inject(name, message)
                graph.run_until_idle()
            composited = graph.poll("composited")

        # Far more tracks than frames, which is the whole point of the hold.
        self.assertEqual(len(tracks), len(self.samples))
        self.assertGreater(len(tracks), 5 * len(composited))
        # The first decimated frame arrives before any track exists, and a hold with
        # nothing held emits nothing.
        self.assertEqual(
            len(composited),
            video_pipeline.FRAME_COUNT // video_pipeline.DECIMATE - 1,
        )

    def test_frames_keep_their_shape_and_dtype_through_the_graph(self):
        with Graph.instantiate(
            video_pipeline.build_blueprint(), video_pipeline.build_registry()
        ) as graph:
            for name, message in self.recording:
                graph.inject(name, message)
                graph.run_until_idle()
            composited = graph.poll("composited")
        for message in composited:
            self.assertEqual(
                message.payload.shape,
                (video_pipeline.HEIGHT, video_pipeline.WIDTH, 3),
            )
            self.assertEqual(message.payload.dtype, np.uint8)

    def test_the_overlay_moves_between_the_first_and_last_frame(self):
        with Graph.instantiate(
            video_pipeline.build_blueprint(), video_pipeline.build_registry()
        ) as graph:
            for name, message in self.recording:
                graph.inject(name, message)
                graph.run_until_idle()
            composited = graph.poll("composited")
        differing = np.any(composited[0].payload != composited[-1].payload, axis=2)
        self.assertGreater(int(np.count_nonzero(differing)), 0)

    def test_replaying_gives_identical_frames(self):
        first = replay(
            video_pipeline.build_blueprint(),
            video_pipeline.build_registry(),
            self.recording,
        )
        second = replay(
            video_pipeline.build_blueprint(),
            video_pipeline.build_registry(),
            self.recording,
        )
        self.assertEqual(len(first["composited"]), len(second["composited"]))
        for a, b in zip(first["composited"], second["composited"]):
            self.assertEqual(a.timestamp, b.timestamp)
            np.testing.assert_array_equal(a.payload, b.payload)

    def test_main_runs(self):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            video_pipeline.main()
        output = buffer.getvalue()
        self.assertIn("Rate mismatch", output)
        self.assertIn("zero-output case", output)


if __name__ == "__main__":
    unittest.main()
