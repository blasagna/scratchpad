"""Which optional dependencies this environment has.

The core is stdlib-only, so the whole test suite has to pass in an environment where
numpy and pyarrow are absent -- which is the environment a future minimal or embedded
port will actually be in. The example tests that need them skip instead of failing.
"""

try:
    import numpy  # noqa: F401

    HAVE_NUMPY = True
except ImportError:  # pragma: no cover - depends on the environment
    HAVE_NUMPY = False

try:
    import pyarrow  # noqa: F401

    HAVE_PYARROW = True
except ImportError:  # pragma: no cover - depends on the environment
    HAVE_PYARROW = False

NUMPY_REASON = "numpy is not installed in this environment"
PYARROW_REASON = "pyarrow is not installed in this environment"
