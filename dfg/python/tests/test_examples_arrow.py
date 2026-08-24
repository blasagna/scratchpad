"""The columnar example, and the one-engine bet it exists to check.

The claim under test is the contract's: real-time and batch are the same runtime, with
different chunk sizes, because a batch is a very large sample. If the aggregates
depended on the chunk size, that would be false.
"""

import io
import unittest
from contextlib import redirect_stdout

from optional import HAVE_PYARROW, PYARROW_REASON

if HAVE_PYARROW:
    import pyarrow as pa

    from dfg.errors import ParamError
    from dfg.graph import Graph
    from dfg.message import Message
    from dfg.validate import check
    from examples import arrow_batch
    from examples.nodes import arrow
    from examples.synth import synth_signal


def sample_message(index=0, **overrides):
    from examples.nodes.signal import Sample

    fields = {"x": 0.0, "y": 0.0, "z": 1.0}
    fields.update(overrides)
    return Message(Sample(**fields), index * 1_000_000)


@unittest.skipUnless(HAVE_PYARROW, PYARROW_REASON)
class TestBatchFromSamples(unittest.TestCase):
    def node(self, rows):
        node = arrow.BatchFromSamples(rows=rows)
        node.setup()
        return node

    def test_emits_nothing_until_the_batch_is_full(self):
        node = self.node(4)
        for i in range(3):
            self.assertEqual(
                node.run(inp=(sample_message(i),)),
                arrow.BatchFromSamples.Out(output=()),
            )
        out = node.run(inp=(sample_message(3),))
        self.assertEqual(len(out.output), 1)

    def test_the_batch_has_the_declared_schema_and_row_count(self):
        node = self.node(3)
        out = node.run(inp=tuple(sample_message(i) for i in range(3)))
        (message,) = out.output
        batch = message.payload
        self.assertIsInstance(batch, pa.RecordBatch)
        self.assertEqual(batch.num_rows, 3)
        self.assertEqual(batch.schema, arrow.SAMPLE_SCHEMA)
        self.assertEqual(batch.column("t_ns").to_pylist(), [0, 1_000_000, 2_000_000])

    def test_the_batch_carries_its_first_rows_sample_time(self):
        node = self.node(3)
        out = node.run(inp=tuple(sample_message(i + 5) for i in range(3)))
        (message,) = out.output
        self.assertEqual(message.timestamp, 5_000_000)

    def test_rows_one_emits_a_batch_per_sample(self):
        # The streaming extreme, and the reason the bet is checkable at all.
        node = self.node(1)
        out = node.run(inp=tuple(sample_message(i) for i in range(4)))
        self.assertEqual(len(out.output), 4)
        self.assertTrue(all(m.payload.num_rows == 1 for m in out.output))

    def test_a_partial_batch_is_dropped_at_teardown(self):
        # teardown is for releasing; producing output there would produce it after
        # the scheduler stopped looking.
        node = self.node(8)
        node.run(inp=tuple(sample_message(i) for i in range(3)))
        node.teardown()
        self.assertEqual(node._rows, [])

    def test_rows_below_one_is_rejected(self):
        with self.assertRaises(ParamError):
            arrow.BatchFromSamples(rows=0)


@unittest.skipUnless(HAVE_PYARROW, PYARROW_REASON)
class TestColumnarNodes(unittest.TestCase):
    def batch(self, z_values):
        columns = {
            "t_ns": list(range(len(z_values))),
            "x": [0.0] * len(z_values),
            "y": [0.0] * len(z_values),
            "z": list(z_values),
        }
        return Message(
            pa.RecordBatch.from_arrays(
                [pa.array(columns[field.name]) for field in arrow.SAMPLE_SCHEMA],
                schema=arrow.SAMPLE_SCHEMA,
            ),
            0,
        )

    def test_filter_keeps_the_passing_rows(self):
        node = arrow.Filter(column="z", op="greater", value=0.0)
        (out,) = node.run(inp=(self.batch([1.0, -1.0, 2.0, -3.0]),)).output
        self.assertEqual(out.payload.num_rows, 2)
        self.assertEqual(out.payload.column("z").to_pylist(), [1.0, 2.0])

    def test_filter_emits_nothing_when_no_row_survives(self):
        # Which saves every downstream node an emptiness check.
        node = arrow.Filter(column="z", op="greater", value=100.0)
        self.assertEqual(node.run(inp=(self.batch([1.0, 2.0]),)).output, ())

    def test_filter_rejects_an_unknown_op(self):
        with self.assertRaises(ParamError):
            arrow.Filter(column="z", op="approximately")

    def test_project_computes_the_magnitude(self):
        node = arrow.Project(keep=["t_ns"], magnitude_name="mag")
        (out,) = node.run(inp=(self.batch([3.0]),)).output
        self.assertEqual(out.payload.schema.names, ["t_ns", "mag"])
        self.assertAlmostEqual(out.payload.column("mag")[0].as_py(), 3.0, places=9)

    def test_aggregate_reduces_a_batch_to_scalars(self):
        project = arrow.Project(keep=["t_ns"], magnitude_name="mag")
        aggregate = arrow.Aggregate(column="mag")
        (projected,) = project.run(inp=(self.batch([3.0, 4.0]),)).output
        (out,) = aggregate.run(inp=(projected,)).output
        self.assertEqual(out.payload["rows"], 2)
        self.assertAlmostEqual(out.payload["sum"], 7.0, places=9)
        self.assertAlmostEqual(out.payload["min"], 3.0, places=9)
        self.assertAlmostEqual(out.payload["max"], 4.0, places=9)

    def test_to_table_accumulates_everything_so_far(self):
        node = arrow.ToTable()
        node.setup()
        first = node.run(inp=(self.batch([1.0, 2.0]),)).output[0]
        second = node.run(inp=(self.batch([3.0]),)).output[0]
        self.assertIsInstance(first.payload, pa.Table)
        self.assertEqual(first.payload.num_rows, 2)
        self.assertEqual(second.payload.num_rows, 3)


@unittest.skipUnless(HAVE_PYARROW, PYARROW_REASON)
class TestArrowPipeline(unittest.TestCase):
    def test_the_blueprint_validates(self):
        self.assertEqual(
            check(arrow_batch.build_blueprint(), arrow_batch.build_registry()), ()
        )

    def test_the_chunk_size_is_one_graph_parameter(self):
        spec = arrow_batch.with_rows(arrow_batch.build_blueprint(), 8)
        graph = Graph.instantiate(spec, arrow_batch.build_registry())
        self.assertEqual(graph.flat.nodes["batch"].params["rows"], 8)

    def test_the_table_has_the_projected_schema(self):
        result = arrow_batch.run(64)
        self.assertIsInstance(result["table"], pa.Table)
        self.assertEqual(result["table"].schema.names, ["t_ns", "mag"])
        self.assertEqual(result["table"].num_rows, result["total_rows"])

    def test_batch_size_one_and_sixty_four_give_identical_aggregates(self):
        # The claim, checked: a batch is a very large sample.
        streaming = arrow_batch.run(1)
        columnar = arrow_batch.run(64)
        for key in ("sum", "min", "max", "total_rows"):
            with self.subTest(key=key):
                self.assertEqual(streaming[key], columnar[key])

    def test_every_chunk_size_agrees(self):
        results = [arrow_batch.run(rows) for rows in (1, 2, 8, 64, 192)]
        self.assertEqual(len({result["sum"] for result in results}), 1)
        self.assertEqual(len({result["min"] for result in results}), 1)
        self.assertEqual(len({result["max"] for result in results}), 1)
        self.assertEqual(len({result["total_rows"] for result in results}), 1)

    def test_the_chunk_size_does_change_how_often_nodes_fire(self):
        # The outputs match; the work does not. That is the difference between a
        # policy and a behaviour.
        streaming = arrow_batch.run(1)
        columnar = arrow_batch.run(192)
        self.assertEqual(streaming["aggregate_firings"], streaming["batches"])
        self.assertEqual(columnar["aggregate_firings"], 1)
        self.assertGreater(
            streaming["aggregate_firings"], columnar["aggregate_firings"]
        )

    def test_the_filter_drops_the_negative_rows_and_the_count_says_so(self):
        # Synthetic z traces a cosine, so it swings negative and `z > 0` keeps only
        # the rows above zero. The kept count is row-wise, so it is the same at every
        # chunk size -- which is what the agreement tests above are really checking.
        expected = sum(
            1
            for message in synth_signal(arrow_batch.SAMPLE_COUNT)
            if message.payload.z > 0
        )
        self.assertEqual(arrow_batch.run(64)["total_rows"], expected)
        self.assertLess(expected, arrow_batch.SAMPLE_COUNT)

    def test_main_runs(self):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            arrow_batch.main()
        output = buffer.getvalue()
        self.assertIn("identical sums:       True", output)
        self.assertIn("pyarrow.lib.Table", output)


if __name__ == "__main__":
    unittest.main()
