"""A recorded input sequence replayed twice, byte for byte.

Run with ``pixi run replay``.

Determinism is what makes offline reprocessing of a recording trustworthy: the same
blueprint plus the same injected sequence produces the same outputs. This script
attacks that from four directions, because the interesting failures are not the
obvious one:

1. **rerun** -- two fresh instantiations of the same blueprint;
2. **reordered blueprint** -- the same graph with its nodes and edges declared in a
   shuffled order. This is what tie-breaking by qualified node ID buys: without it,
   two authors who wrote the same graph in a different order would get different
   output;
3. **different ordering policy** -- ``topological`` and ``level`` fire in different
   orders and must still agree on what came out;
4. **different wall clock** -- a replay running far faster than real time must not
   change a thing, which is why latency is measured against a separate clock and
   never against a message's sample timestamp.
"""

from __future__ import annotations

import hashlib
import random

from dfg.blueprint import GraphSpec
from dfg.graph import replay
from examples.imu_pipeline import build_blueprint, build_registry
from examples.synth import tracker_recording

SAMPLE_COUNT = 24


class SteppedClock:
    """A wall clock under our control, so "faster than real time" is testable."""

    def __init__(self, start: int = 0, step: int = 1) -> None:
        self.now = start
        self.step = step

    def __call__(self) -> int:
        value = self.now
        self.now += self.step
        return value


def digest(outputs) -> str:
    """A hash over every output message's payload *and* sample timestamp.

    Hashing the payloads alone would let a run that lost the sample times pass, and
    losing the sample times is exactly the kind of bug this is meant to catch.
    """
    hasher = hashlib.sha256()
    for name in sorted(outputs):
        hasher.update(name.encode())
        for message in outputs[name]:
            hasher.update(f"{message.timestamp}:{message.payload}".encode())
    return hasher.hexdigest()[:16]


def shuffled(spec: GraphSpec, *, seed: int) -> GraphSpec:
    """The same blueprint with node and edge declaration order shuffled."""
    rng = random.Random(seed)
    nodes, edges = list(spec.nodes), list(spec.edges)
    rng.shuffle(nodes)
    rng.shuffle(edges)
    return GraphSpec(
        name=spec.name,
        nodes=tuple(nodes),
        edges=tuple(edges),
        inputs=spec.inputs,
        outputs=spec.outputs,
        params=spec.params,
    )


def main() -> None:
    spec = build_blueprint()
    recording = tracker_recording(SAMPLE_COUNT)

    print(
        f"Recording: {len(recording)} injections into "
        f"{len({name for name, _ in recording})} graph inputs"
    )
    print(f"Blueprint: {spec.name}, {len(spec.nodes)} top-level nodes")
    print()

    runs: list[tuple[str, str]] = []

    first = replay(spec, build_registry(), recording)
    second = replay(spec, build_registry(), recording)
    runs.append(("run 1", digest(first)))
    runs.append(("run 2", digest(second)))

    for seed in (1, 2, 3):
        variant = shuffled(spec, seed=seed)
        runs.append(
            (
                f"shuffled blueprint (seed {seed})",
                digest(replay(variant, build_registry(), recording)),
            )
        )

    for ordering in ("topological", "level", "priority"):
        runs.append(
            (
                f"ordering={ordering}",
                digest(replay(spec, build_registry(), recording, ordering=ordering)),
            )
        )

    runs.append(
        (
            "clock 1e6x faster",
            digest(
                replay(
                    spec,
                    build_registry(),
                    recording,
                    clock=SteppedClock(start=10**15, step=10**6),
                )
            ),
        )
    )

    print("Output digest (payloads and sample timestamps) per run")
    print("=" * 74)
    for label, value in runs:
        print(f"  {label:<32} {value}")
    print()

    unique = {value for _, value in runs}
    print(f"  distinct digests: {len(unique)}")
    if len(unique) == 1:
        print("  Every run produced identical output.")
    else:
        raise AssertionError(f"replay is not deterministic: {unique}")
    print()

    print(f"Messages out: {len(first['pose'])} on 'pose'")
    print(f"  first: {first['pose'][0].timestamp} ns  {first['pose'][0].payload}")
    print(f"  last:  {first['pose'][-1].timestamp} ns  {first['pose'][-1].payload}")
    print()
    print("The shuffled blueprints are the load-bearing case. A topological order is")
    print("not unique and neither is the set of ready nodes at a level, so without")
    print("breaking ties by qualified node ID these rows would legitimately differ.")


if __name__ == "__main__":
    main()
