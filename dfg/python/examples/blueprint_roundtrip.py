"""A blueprint through JSON and a fresh registry, then run to the same output.

Run with ``pixi run roundtrip``.

Three things are being shown, and the third is the interesting one:

1. a blueprint serializes to JSON and reloads to an *equal* blueprint;
2. re-serializing is byte-identical, so a diff of two blueprints shows only what
   actually changed;
3. **loading needs no registry.** The reloaded blueprint is a complete description
   that nothing can instantiate until a registry resolves its type names -- which is
   why the registry is a concept in the contract and not an implementation detail.
   The script proves it by trying to instantiate with an empty registry first.
"""

from __future__ import annotations

from dfg.errors import ValidationError
from dfg.graph import Graph, replay
from dfg.registry import Registry
from dfg.serialize import dumps, loads
from examples.reading_pipeline import build_blueprint, build_registry
from examples.synth import reading_recording

SAMPLE_COUNT = 12


def digest(outputs) -> list[str]:
    """A comparable summary of a replay's output."""
    return [f"{m.timestamp}:{m.payload}" for m in outputs["result"]]


def excerpt(text: str, *, head: int = 26, tail: int = 8) -> str:
    """The head and tail of the JSON, so the shape is visible without the bulk."""
    lines = text.splitlines()
    if len(lines) <= head + tail:
        return text
    skipped = len(lines) - head - tail
    return "\n".join([*lines[:head], f"  ... {skipped} lines ...", *lines[-tail:]])


def main() -> None:
    original = build_blueprint()
    text = dumps(original)

    print("The processor blueprint as JSON")
    print("=" * 74)
    print(excerpt(text))
    print()

    reloaded = loads(text)
    print("Round-trip")
    print("=" * 74)
    print(f"  reloaded == original:        {reloaded == original}")
    print(f"  re-serialized byte-for-byte: {dumps(reloaded) == text}")
    print(f"  JSON size:                   {len(text)} bytes")
    print()

    print("Loading needs no registry; instantiating does")
    print("=" * 74)
    try:
        Graph.instantiate(reloaded, Registry())
    except ValidationError as exc:
        codes = sorted({problem.code for problem in exc.problems})
        print(f"  with an empty registry: ValidationError, codes {codes}")
        print(f"  problems reported:      {len(exc.problems)} (all of them, at once)")
    else:
        raise AssertionError("an empty registry was expected to fail")

    registry = build_registry()
    print(f"  with the real registry: {Graph.instantiate(reloaded, registry)}")
    print()

    print("Both blueprints produce the same output from the same recording")
    print("=" * 74)
    recording = reading_recording(SAMPLE_COUNT)
    from_original = replay(original, build_registry(), recording)
    from_reloaded = replay(reloaded, build_registry(), recording)
    print(
        f"  messages out:  {len(from_original['result'])} and {len(from_reloaded['result'])}"
    )
    print(f"  identical:     {digest(from_original) == digest(from_reloaded)}")
    print()
    print("  first output, from the reloaded blueprint:")
    print(f"    {from_reloaded['result'][0].payload}")


if __name__ == "__main__":
    main()
