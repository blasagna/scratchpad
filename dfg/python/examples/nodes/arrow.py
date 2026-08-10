"""Columnar nodes over pyarrow record batches and tables.

This is where the contract's biggest bet gets tested. Treating a batch as a very
large sample keeps one node API and one set of semantics, and it is why this is one
runtime and not two. The bet is real: a columnar engine over Arrow tables wants to
schedule by operator fusion and vectorized passes, which is not what a
message-passing scheduler does.

What these nodes show is the part that does hold up. The same 200 Hz IMU stream that
:mod:`examples.nodes.imu` processes one dataclass at a time is accumulated into
record batches here and filtered, projected, and aggregated column-wise -- with the
same ``run`` signature, the same readiness rules, and the same scheduler. A batch is
just a payload that happens to hold many rows.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, ClassVar

import pyarrow as pa
import pyarrow.compute as pc

from dfg.errors import ParamError
from dfg.message import Message
from dfg.node import REQUIRED, Inputs, Node, Outputs
from dfg.ports import PortSpec
from dfg.registry import Registry

RECORD_BATCH = "record_batch"
TABLE = "table"

IMU_SCHEMA = pa.schema(
    [
        pa.field("t_ns", pa.int64()),
        pa.field("ax", pa.float64()),
        pa.field("ay", pa.float64()),
        pa.field("az", pa.float64()),
        pa.field("gx", pa.float64()),
        pa.field("gy", pa.float64()),
        pa.field("gz", pa.float64()),
    ]
)


class BatchFromSamples(Node):
    """Accumulates :class:`~examples.nodes.imu.ImuSample` messages into a batch.

    The row-to-column boundary, and the one place the two worlds meet. Emits a batch
    every ``rows`` messages and nothing in between, so it is another zero-or-more
    node -- and with ``rows=1`` it emits one batch per sample, which is what makes the
    "a batch is a very large sample" claim checkable rather than rhetorical.

    The sample timestamp of an emitted batch is its *first* row's, matching
    :class:`examples.nodes.core.Window`.
    """

    INPUTS = (PortSpec("input", type_tag="ImuSample"),)
    OUTPUTS = (PortSpec("output", type_tag=RECORD_BATCH),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"rows": REQUIRED}

    def __init__(self, **params: Any) -> None:
        super().__init__(**params)
        if self.params["rows"] < 1:
            raise ParamError(f"rows must be at least 1, got {self.params['rows']}")

    def setup(self) -> None:
        self._rows: list[tuple[int, float, float, float, float, float, float]] = []

    def run(self, inputs: Inputs) -> Outputs:
        wanted = self.params["rows"]
        batches: list[Message[pa.RecordBatch]] = []
        for message in inputs.get("input", ()):
            sample = message.payload
            self._rows.append(
                (
                    message.timestamp,
                    sample.ax,
                    sample.ay,
                    sample.az,
                    sample.gx,
                    sample.gy,
                    sample.gz,
                )
            )
            if len(self._rows) == wanted:
                batches.append(self._emit())
        return {"output": batches}

    def teardown(self) -> None:
        # A partial batch is dropped rather than emitted: teardown is for releasing,
        # and a node that produced output there would produce it after the scheduler
        # had stopped looking.
        self._rows.clear()

    def _emit(self) -> Message[pa.RecordBatch]:
        columns = list(zip(*self._rows))
        batch = pa.RecordBatch.from_arrays(
            [pa.array(column) for column in columns], schema=IMU_SCHEMA
        )
        timestamp = self._rows[0][0]
        self._rows = []
        return Message(batch, timestamp)


class Filter(Node):
    """Keeps rows where a column passes a threshold, using ``pyarrow.compute``.

    Emits nothing when a batch has no surviving rows, which is the honest answer and
    saves every downstream node an emptiness check.
    """

    INPUTS = (PortSpec("input", type_tag=RECORD_BATCH),)
    OUTPUTS = (PortSpec("output", type_tag=RECORD_BATCH),)
    PARAMS: ClassVar[Mapping[str, Any]] = {
        "column": REQUIRED,
        "op": "greater",
        "value": 0.0,
    }

    OPS: ClassVar[Mapping[str, Any]] = {
        "greater": pc.greater,
        "greater_equal": pc.greater_equal,
        "less": pc.less,
        "less_equal": pc.less_equal,
    }

    def __init__(self, **params: Any) -> None:
        super().__init__(**params)
        if self.params["op"] not in self.OPS:
            raise ParamError(
                f"op must be one of {sorted(self.OPS)}, got {self.params['op']!r}"
            )

    def run(self, inputs: Inputs) -> Outputs:
        compare = self.OPS[self.params["op"]]
        column, value = self.params["column"], self.params["value"]
        out: list[Message[pa.RecordBatch]] = []
        for message in inputs.get("input", ()):
            batch = message.payload
            mask = compare(batch.column(column), value)
            filtered = batch.filter(mask)
            if filtered.num_rows:
                out.append(message.with_payload(filtered))
        return {"output": out}


class Project(Node):
    """Selects columns and adds a computed accelerometer magnitude."""

    INPUTS = (PortSpec("input", type_tag=RECORD_BATCH),)
    OUTPUTS = (PortSpec("output", type_tag=RECORD_BATCH),)
    PARAMS: ClassVar[Mapping[str, Any]] = {
        "keep": ("t_ns",),
        "magnitude_name": "accel_magnitude",
    }

    def run(self, inputs: Inputs) -> Outputs:
        keep = list(self.params["keep"])
        name = self.params["magnitude_name"]
        out: list[Message[pa.RecordBatch]] = []
        for message in inputs.get("input", ()):
            batch = message.payload
            squares = pc.add(
                pc.add(
                    pc.power(batch.column("ax"), 2), pc.power(batch.column("ay"), 2)
                ),
                pc.power(batch.column("az"), 2),
            )
            magnitude = pc.sqrt(squares)
            arrays = [batch.column(column) for column in keep] + [magnitude]
            out.append(
                message.with_payload(
                    pa.RecordBatch.from_arrays(arrays, names=[*keep, name])
                )
            )
        return {"output": out}


class Aggregate(Node):
    """Reduces a batch to one row of scalars.

    A whole batch in, a handful of numbers out. Because these are plain Python floats,
    the result is comparable across batch sizes -- which is exactly what the
    ``rows=1`` versus ``rows=64`` comparison needs.
    """

    INPUTS = (PortSpec("input", type_tag=RECORD_BATCH),)
    OUTPUTS = (PortSpec("output", type_tag="aggregate"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"column": "accel_magnitude"}

    def run(self, inputs: Inputs) -> Outputs:
        column = self.params["column"]
        out: list[Message[dict[str, Any]]] = []
        for message in inputs.get("input", ()):
            values = message.payload.column(column)
            out.append(
                message.with_payload(
                    {
                        "rows": message.payload.num_rows,
                        "sum": float(pc.sum(values).as_py()),
                        "min": float(pc.min(values).as_py()),
                        "max": float(pc.max(values).as_py()),
                    }
                )
            )
        return {"output": out}


class ToTable(Node):
    """Concatenates every batch it has seen into one table, on demand.

    Emits a table per firing, each holding everything so far -- so the last one is the
    whole recording. This is the shape an offline batch job actually wants, and it
    only works because a node is allowed to keep state.
    """

    INPUTS = (PortSpec("input", type_tag=RECORD_BATCH),)
    OUTPUTS = (PortSpec("output", type_tag=TABLE),)

    def setup(self) -> None:
        self._batches: list[pa.RecordBatch] = []

    def run(self, inputs: Inputs) -> Outputs:
        out: list[Message[pa.Table]] = []
        for message in inputs.get("input", ()):
            self._batches.append(message.payload)
            out.append(message.with_payload(pa.Table.from_batches(list(self._batches))))
        return {"output": out}


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(BatchFromSamples)
    registry.register(Filter)
    registry.register(Project)
    registry.register(Aggregate)
    registry.register(ToTable)
    return registry
