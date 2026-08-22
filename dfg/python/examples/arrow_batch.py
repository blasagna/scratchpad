"""Columnar batches through the same runtime: the one-engine bet, checked.

Run with ``pixi run arrow``.

The contract makes a bet worth stating plainly: **real-time and batch are the same
runtime**, with different schedulers and chunk sizes, because a batch is a very large
sample. That keeps one node API and one set of semantics to test. It is also a real
bet, because a columnar engine over Arrow tables wants to schedule by operator fusion
and vectorized passes, which is not what a message-passing scheduler does.

This script checks the part that is checkable. The same blueprint runs at
``rows=1`` -- a batch per sample, the streaming extreme -- and at ``rows=64``, a real
columnar chunk, and the aggregates come out the same::

    signal_raw -> batch(rows) -> filter -> project -> aggregate -> summary
                                                 \\-> to_table -> table

If the numbers agreed only at one chunk size, the bet would already have failed.
"""

from __future__ import annotations

import pyarrow as pa

from dfg.blueprint import GraphBuilder, GraphSpec, ParamRef
from dfg.graph import Graph
from dfg.registry import Registry
from examples.nodes import arrow, core
from examples.synth import synth_signal

SAMPLE_COUNT = 192


def build_registry() -> Registry:
    registry = Registry()
    core.register(registry)
    arrow.register(registry)
    return registry


def build_blueprint() -> GraphSpec:
    """The columnar chain. ``rows`` is a graph parameter, so it is one knob."""
    builder = GraphBuilder("columnar", params={"rows": 64})
    builder.add("batch", arrow.BatchFromSamples, params={"rows": ParamRef("rows")})
    builder.add(
        "filter",
        arrow.Filter,
        params={"column": "z", "op": "greater", "value": 0.0},
    )
    builder.add(
        "project", arrow.Project, params={"keep": ["t_ns"], "magnitude_name": "mag"}
    )
    builder.add("aggregate", arrow.Aggregate, params={"column": "mag"})
    builder.add("table", arrow.ToTable)

    builder.connect("batch.output", "filter.inp")
    builder.connect("filter.output", "project.inp")
    builder.connect("project.output", "aggregate.inp")
    builder.connect("project.output", "table.inp")

    builder.add_input("signal_raw", "batch.inp", type_tag="Sample")
    builder.add_output("summary", "aggregate.output")
    builder.add_output("table", "table.output")
    return builder.build()


def with_rows(spec: GraphSpec, rows: int) -> GraphSpec:
    """The same blueprint with a different chunk size. One parameter, nothing else."""
    return GraphSpec(
        name=spec.name,
        nodes=spec.nodes,
        edges=spec.edges,
        inputs=spec.inputs,
        outputs=spec.outputs,
        params={**spec.params, "rows": rows},
    )


def run(rows: int) -> dict:
    """Run the chain at a given chunk size and total up the aggregates."""
    spec = with_rows(build_blueprint(), rows)
    samples = synth_signal(SAMPLE_COUNT)
    with Graph.instantiate(spec, build_registry()) as graph:
        for message in samples:
            graph.inject("signal_raw", message)
            graph.run_until_idle()
        summaries = graph.poll("summary")
        tables = graph.poll("table")
        fired = graph.control.node_stats()["aggregate"].fired

    total_rows = sum(s.payload["rows"] for s in summaries)
    return {
        "rows_per_batch": rows,
        "batches": len(summaries),
        "aggregate_firings": fired,
        "total_rows": total_rows,
        # Totalled across batches, so the answer does not depend on how the rows
        # were grouped -- which is the whole question being asked.
        "sum": round(sum(s.payload["sum"] for s in summaries), 9),
        "min": round(min(s.payload["min"] for s in summaries), 9),
        "max": round(max(s.payload["max"] for s in summaries), 9),
        "table": tables[-1].payload if tables else None,
    }


def main() -> None:
    print(f"{SAMPLE_COUNT} signal samples, as dataclasses on the way in")
    print()

    table_shown = False
    results = []
    for rows in (1, 8, 64, SAMPLE_COUNT):
        result = run(rows)
        results.append(result)
        if not table_shown and result["table"] is not None:
            table = result["table"]
            print("The columnar payload")
            print("=" * 74)
            print(f"  type:     {type(table).__module__}.{type(table).__name__}")
            print(f"  num_rows: {table.num_rows}")
            print(f"  schema:   {str(table.schema).replace(chr(10), ', ')}")
            print(
                f"  first 3 rows of 'mag': "
                f"{[round(v, 6) for v in table.column('mag')[:3].to_pylist()]}"
            )
            print()
            table_shown = True

    print("The same blueprint at four chunk sizes")
    print("=" * 74)
    header = f"  {'rows/batch':>10}  {'batches':>7}  {'total rows':>10}  {'sum':>14}  {'max':>10}"
    print(header)
    for result in results:
        print(
            f"  {result['rows_per_batch']:>10}  {result['batches']:>7}  "
            f"{result['total_rows']:>10}  {result['sum']:>14.6f}  "
            f"{result['max']:>10.6f}"
        )
    print()

    sums = {result["sum"] for result in results}
    mins = {result["min"] for result in results}
    maxes = {result["max"] for result in results}
    rows_seen = {result["total_rows"] for result in results}
    print(f"  identical sums:       {len(sums) == 1}")
    print(f"  identical min/max:    {len(mins) == 1 and len(maxes) == 1}")
    print(f"  identical row counts: {len(rows_seen) == 1}  ({rows_seen.pop()} rows)")
    print()
    print("  rows=1 is the streaming extreme -- a batch per sample, and the")
    print(
        f"  aggregate node fires {results[0]['aggregate_firings']} times. "
        f"rows={SAMPLE_COUNT} is the batch extreme: one"
    )
    print(
        f"  chunk, {results[-1]['aggregate_firings']} firing. Same blueprint, same "
        "scheduler, same numbers."
    )
    print()

    print("What this does and does not prove")
    print("=" * 74)
    print("  It shows that one node contract covers both ends, which is the claim")
    print("  worth having: there is one set of semantics to test, and an offline")
    print("  reprocess of a recording runs the code that ran live.")
    print()
    print("  It does not show that this is the *fast* way to do columnar work. A")
    print("  real columnar engine fuses operators and makes vectorized passes over")
    print("  whole tables; here `filter`, `project`, and `aggregate` are separate")
    print("  nodes with a queue between each pair, and the scheduler moves one")
    print("  message at a time. If that bet turns out wrong, the blueprint layer is")
    print("  the part that survives.")
    print()
    print(
        f"  (Confirming the payload really is Arrow: "
        f"{isinstance(results[0]['table'], pa.Table)})"
    )


if __name__ == "__main__":
    main()
