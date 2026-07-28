"""Tests for the Python side of the bindings.

These deliberately do *not* re-test the statistics: that is `cargo test -p
statkit_core`'s job, and repeating it here would only prove the arithmetic twice.
What is tested is the seam -- that values cross into Rust and back with the right
types, that Rust errors arrive as Python exceptions, and that the CLI is wired up.
"""

import doctest
import io
import pickle
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import statkit
from statkit import cli

SAMPLE = [1.0, 2.0, 3.0, 4.0, 10.0]


class TestSummarize(unittest.TestCase):
    def test_returns_a_summary_with_float_fields(self):
        summary = statkit.summarize(SAMPLE)
        self.assertIsInstance(summary, statkit.Summary)
        self.assertEqual(summary.count, 5)
        self.assertIsInstance(summary.count, int)
        self.assertAlmostEqual(summary.mean, 4.0)
        self.assertAlmostEqual(summary.median, 3.0)
        self.assertAlmostEqual(summary.min, 1.0)
        self.assertAlmostEqual(summary.max, 10.0)
        self.assertAlmostEqual(summary.stddev, 12.5**0.5)

    def test_summary_is_immutable(self):
        summary = statkit.summarize(SAMPLE)
        with self.assertRaises(AttributeError):
            summary.mean = 0.0

    def test_repr_round_trips_the_numbers(self):
        self.assertEqual(
            repr(statkit.summarize([1.0, 3.0])),
            "Summary(count=2, mean=2.0, median=2.0, min=1.0, max=3.0, "
            "stddev=1.4142135623730951)",
        )

    def test_accepts_ints_and_one_shot_iterables(self):
        from_list = statkit.summarize([1, 2, 3])
        from_generator = statkit.summarize(x for x in (1, 2, 3))
        self.assertEqual(from_list.mean, from_generator.mean)
        self.assertEqual(from_list.count, 3)

    def test_as_dict_exposes_every_field(self):
        fields = statkit.as_dict(statkit.summarize(SAMPLE))
        self.assertEqual(
            sorted(fields), ["count", "max", "mean", "median", "min", "stddev"]
        )


class TestZscores(unittest.TestCase):
    def test_returns_a_plain_list_of_floats(self):
        scores = statkit.zscores([1.0, 2.0, 3.0])
        self.assertIsInstance(scores, list)
        self.assertEqual(len(scores), 3)
        self.assertAlmostEqual(sum(scores), 0.0)
        self.assertAlmostEqual(scores[0], -1.0)


class TestParseValues(unittest.TestCase):
    def test_splits_on_whitespace_and_commas(self):
        self.assertEqual(statkit.parse_values(" 1, 2,,3\n-4.5 "), [1.0, 2.0, 3.0, -4.5])

    def test_blank_text_is_empty(self):
        self.assertEqual(statkit.parse_values("  \n , "), [])


class TestErrors(unittest.TestCase):
    """Every Rust `StatError` variant must surface as a catchable Python exception."""

    def test_stat_error_is_an_exception_subclass(self):
        self.assertTrue(issubclass(statkit.StatError, Exception))

    def test_empty_input(self):
        with self.assertRaises(statkit.StatError) as caught:
            statkit.summarize([])
        self.assertEqual(str(caught.exception), "no values given")

    def test_non_finite_input(self):
        with self.assertRaises(statkit.StatError):
            statkit.summarize([1.0, float("nan")])

    def test_constant_input_has_no_zscores(self):
        with self.assertRaises(statkit.StatError):
            statkit.zscores([2.0, 2.0, 2.0])

    def test_overflowing_input_is_rejected_rather_than_returning_inf(self):
        with self.assertRaises(statkit.StatError):
            statkit.summarize([1e308, 1e308])
        with self.assertRaises(statkit.StatError):
            statkit.zscores([1e200, -1e200])

    def test_exception_is_fully_qualified_and_picklable(self):
        # `create_exception!` stringifies its first argument into __module__;
        # a bare `_core` there would name a module Python cannot import, and
        # pickle (hence multiprocessing) would refuse the exception.
        self.assertEqual(statkit.StatError.__module__, "statkit._core")
        restored = pickle.loads(pickle.dumps(statkit.StatError("boom")))
        self.assertIsInstance(restored, statkit.StatError)

    def test_unparsable_token_is_named(self):
        with self.assertRaises(statkit.StatError) as caught:
            statkit.parse_values("1 two 3")
        self.assertEqual(str(caught.exception), "not a number: 'two'")

    def test_non_numeric_python_input_stays_a_python_error(self):
        # The wrapper coerces before the extension ever sees the values, so this
        # is a plain TypeError, not a StatError.
        with self.assertRaises(TypeError):
            statkit.summarize([1.0, None])


class TestCli(unittest.TestCase):
    def _run(self, argv, stdin=""):
        """Call cli.main in-process, returning (exit code, stdout)."""
        buffer = io.StringIO()
        original_stdin = sys.stdin
        sys.stdin = io.StringIO(stdin)
        try:
            with redirect_stdout(buffer):
                code = cli.main(argv)
        finally:
            sys.stdin = original_stdin
        return code, buffer.getvalue()

    def test_summary_table_from_stdin(self):
        code, output = self._run([], stdin="1 2 3\n")
        self.assertEqual(code, 0)
        self.assertEqual(
            output,
            "count   3\n"
            "mean    2.000000\n"
            "median  2.000000\n"
            "min     1.000000\n"
            "max     3.000000\n"
            "stddev  1.000000\n",
        )

    def test_zscores_flag(self):
        code, output = self._run(["--zscores"], stdin="1,2,3")
        self.assertEqual(code, 0)
        self.assertEqual(output, "-1.000000\n0.000000\n1.000000\n")

    def test_reads_a_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "numbers.txt"
            path.write_text("4 8\n", encoding="utf-8")
            code, output = self._run([str(path)])
        self.assertEqual(code, 0)
        self.assertIn("mean    6.000000\n", output)

    def test_undecodable_file_reports_cleanly(self):
        """A UnicodeDecodeError is a ValueError, so it needs catching explicitly."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "binary.dat"
            path.write_bytes(b"\x80\xff")
            code, output = self._run([str(path)])
        self.assertEqual(code, 1)
        self.assertEqual(output, "")

    def test_reports_errors_and_exits_nonzero(self):
        code, output = self._run([], stdin="")
        self.assertEqual(code, 1)
        self.assertEqual(output, "")

    def test_module_entry_point(self):
        result = subprocess.run(
            [sys.executable, "-m", "statkit", "--zscores"],
            input="1 2 3",
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(result.stdout, "-1.000000\n0.000000\n1.000000\n")


def load_tests(loader, tests, ignore):
    """Also run the examples in statkit's docstrings, so they cannot rot."""
    tests.addTests(doctest.DocTestSuite(statkit))
    return tests


if __name__ == "__main__":
    unittest.main()
