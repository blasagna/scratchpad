"""Descriptive statistics, computed in Rust.

The compiled extension lives at :mod:`statkit._core` and is deliberately private.
This module is the public surface: it re-exports the types, and wraps the
functions in a thin Python layer that is friendlier than the raw binding.

    >>> import statkit
    >>> statkit.summarize([1, 2, 3, 4, 10])
    Summary(count=5, mean=4.0, median=3.0, min=1.0, max=10.0, stddev=3.5355339059327378)
    >>> statkit.zscores(x / 2 for x in range(4))
    [-1.161895003862225, -0.3872983346207417, 0.3872983346207417, 1.161895003862225]
"""

from collections.abc import Iterable

from . import _core
from ._core import StatError, Summary

__all__ = [
    "StatError",
    "Summary",
    "as_dict",
    "parse_values",
    "summarize",
    "zscores",
]


def summarize(values: Iterable[float]) -> Summary:
    """Return the summary statistics of ``values``.

    Raises:
        StatError: if ``values`` is empty or contains a non-finite number.
    """
    return _core.summarize(_as_floats(values))


def zscores(values: Iterable[float]) -> list[float]:
    """Return each value's distance from the mean, in standard deviations.

    Raises:
        StatError: as :func:`summarize`, and also when every value is identical,
            since the spread is then zero.
    """
    return _core.zscores(_as_floats(values))


def parse_values(text: str) -> list[float]:
    """Parse whitespace- and comma-separated numbers out of ``text``.

    Raises:
        StatError: naming the first token that is not a number.
    """
    return _core.parse_values(text)


def as_dict(summary: Summary) -> dict[str, float]:
    """Return ``summary`` as a plain dict.

    ``Summary`` is a frozen Rust type with no ``__dict__``; this is the Python
    layer's job, not the binding's.
    """
    return {
        "count": summary.count,
        "mean": summary.mean,
        "median": summary.median,
        "min": summary.min,
        "max": summary.max,
        "stddev": summary.stddev,
    }


def _as_floats(values: Iterable[float]) -> list[float]:
    """Materialize any iterable of numbers into the list the extension wants.

    The Rust side takes a ``Vec<f64>``, which PyO3 extracts from a *sequence*.
    Generators and other one-shot iterables are not sequences, so converting here
    is what lets ``statkit.zscores(x for x in ...)`` work at all.
    """
    return [float(value) for value in values]
