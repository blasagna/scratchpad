"""The example node library, grouped by what each module depends on.

* :mod:`examples.nodes.core` -- stdlib only. Generic plumbing.
* :mod:`examples.nodes.reading` -- stdlib only. A scalar dataclass of primitive fields.
* :mod:`examples.nodes.signal` -- stdlib only. A multi-channel ``Sample`` the numpy
  and pyarrow examples accumulate.
* :mod:`examples.nodes.audio` -- numpy. Blocks of samples as ``ndarray``.
* :mod:`examples.nodes.video` -- numpy. Frames as ``H x W x 3`` uint8 ``ndarray``.
* :mod:`examples.nodes.arrow` -- pyarrow. Columnar record batches and tables.

A node type name is serialized into a blueprint, so it is API: keep it short and
stable.
"""
