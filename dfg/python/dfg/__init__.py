"""dfg -- a dataflow graph framework for real-time and batch processing.

``../README.md`` is the contract; this package is its first implementation. The
package is deliberately split along the line that contract draws:

* the **blueprint layer** (:mod:`dfg.blueprint`, :mod:`dfg.validate`,
  :mod:`dfg.flatten`, :mod:`dfg.serialize`, :mod:`dfg.mermaid`) describes,
  validates, serializes, and draws a graph, and holds no runtime state;
* the **runtime layer** (:mod:`dfg.graph`, :mod:`dfg.scheduler`,
  :mod:`dfg.transport`, :mod:`dfg.control`) instantiates a blueprint and actually
  processes data.

The blueprint layer never imports the runtime layer, and the whole package
imports nothing but the standard library. Both are enforced by
``tests/test_core_is_stdlib_only.py`` rather than left to good intentions.
"""

from dfg.errors import (
    DfgError,
    EdgeOverflowError,
    ImmutableParamError,
    LifecycleError,
    NodeContractError,
    NodeRunError,
    NodeSetupError,
    ParamError,
    SchemaVersionError,
    SerializationError,
    UnknownNodeTypeError,
    UnknownReadinessError,
    UnknownTransportError,
    ValidationError,
)
from dfg.message import (
    NANOS_PER_SECOND,
    Message,
    Timestamp,
    ts_from_sample_index,
    ts_from_seconds,
    ts_to_seconds,
)
from dfg.ports import ERROR_PORT, PortSpec, qualify, topic_of

__all__ = [
    "ERROR_PORT",
    "NANOS_PER_SECOND",
    "DfgError",
    "EdgeOverflowError",
    "ImmutableParamError",
    "LifecycleError",
    "Message",
    "NodeContractError",
    "NodeRunError",
    "NodeSetupError",
    "ParamError",
    "PortSpec",
    "SchemaVersionError",
    "SerializationError",
    "Timestamp",
    "UnknownNodeTypeError",
    "UnknownReadinessError",
    "UnknownTransportError",
    "ValidationError",
    "qualify",
    "topic_of",
    "ts_from_sample_index",
    "ts_from_seconds",
    "ts_to_seconds",
]
