"""Type stubs for the compiled extension module.

A ``.so`` carries no signatures a type checker or editor can read, so the stubs
are written by hand and kept in step with ``bindings/src/lib.rs``. The adjacent
``py.typed`` marker is what tells type checkers to look here at all.
"""

from collections.abc import Sequence

class StatError(Exception):
    """Raised when a statistic cannot be computed from the given input."""

class Summary:
    """Summary statistics for a non-empty sample. Immutable."""

    @property
    def count(self) -> int: ...
    @property
    def mean(self) -> float: ...
    @property
    def median(self) -> float: ...
    @property
    def min(self) -> float: ...
    @property
    def max(self) -> float: ...
    @property
    def stddev(self) -> float:
        """Sample standard deviation (``n - 1``); ``0.0`` for a single value."""

def summarize(values: Sequence[float]) -> Summary: ...
def zscores(values: Sequence[float]) -> list[float]: ...
def parse_values(text: str) -> list[float]: ...
