"""Command line interface for :mod:`statkit`.

This is the Python twin of ``core/src/main.rs``: same flags, same output, but
built on the bindings instead of on the Rust library directly. Running both over
the same input is the quickest way to see that the binding is faithful.
"""

import argparse
import sys

from . import StatError, Summary, parse_values, summarize, zscores


def main(argv: list[str] | None = None) -> int:
    """Run the CLI and return the process exit code.

    Returning the code instead of calling :func:`sys.exit` keeps this callable
    from tests.
    """
    parser = argparse.ArgumentParser(
        prog="statkit-py",
        description="Summarizes a list of numbers read from a file or standard input.",
        epilog="Numbers may be separated by any mix of whitespace and commas.",
    )
    parser.add_argument(
        "--zscores",
        action="store_true",
        help="print each value's z-score instead of the summary table",
    )
    parser.add_argument(
        "file",
        nargs="?",
        help="file to read numbers from; standard input is used when omitted",
    )
    # argparse exits with status 2 on a usage error, matching clap and the rest
    # of this repo.
    args = parser.parse_args(argv)

    try:
        text = _read_input(args.file)
        values = parse_values(text)
        if args.zscores:
            output = format_zscores(zscores(values))
        else:
            output = format_summary(summarize(values))
    # UnicodeDecodeError is a ValueError, not an OSError, so a non-UTF-8 file
    # would traceback rather than report cleanly if it were left out.
    except (StatError, OSError, UnicodeDecodeError) as err:
        print(f"statkit: {err}", file=sys.stderr)
        return 1

    print(output, end="")
    return 0


def _read_input(path: str | None) -> str:
    if path is None:
        return sys.stdin.read()
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def format_summary(summary: Summary) -> str:
    """Keep this format in lock-step with ``format_summary`` in ``core/src/main.rs``."""
    floats = (
        ("mean", summary.mean),
        ("median", summary.median),
        ("min", summary.min),
        ("max", summary.max),
        ("stddev", summary.stddev),
    )
    lines = [f"{'count':<8}{summary.count}"]
    lines += [f"{label:<8}{value:.6f}" for label, value in floats]
    return "".join(f"{line}\n" for line in lines)


def format_zscores(scores: list[float]) -> str:
    """Keep this format in lock-step with ``format_zscores`` in ``core/src/main.rs``."""
    return "".join(f"{score:.6f}\n" for score in scores)
