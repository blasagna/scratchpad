# dfg

A dataflow graph framework meant to run the same node code in real-time and in batch,
eventually as a portable native core with language bindings. Ports live in
per-language subdirectories; `python/` is the first and currently the only one.

**[`README.md`](README.md) is the contract.** It was written before any port existed
so that the ports have something to agree with, and it states the alternatives it
rejects rather than only the choices it makes. Its "Contracts" section is the part an
implementation must not vary — a port that differs there is a different framework. A
behaviour change lands in `README.md` first, then in every port.

## Invariants a change must not break

- **The core is stdlib-only.** `python/dfg/` imports nothing but the standard library;
  numpy and pyarrow live under `python/examples/`. Enforced by
  `tests/test_core_is_stdlib_only.py`, which walks the imports with `ast` — that is
  *stronger* than splitting the pixi environment, because it catches a stray
  `import numpy` even in an environment where numpy is installed, which the examples
  need it to be. The same test asserts the suite's other structural rule:
- **The blueprint layer never imports the runtime layer.** `blueprint`, `validate`,
  `flatten`, `serialize`, `mermaid`, `ports`, `readiness`, and `errors` must stay
  usable with no runtime — that is requirement 5's "the two layers never merge", and
  the Tensions section's bet that the blueprint is what survives if the one-engine
  bet turns out wrong. This is why `validate` does **not** check an edge's transport
  name: resolving it needs the transport registry, which is runtime, so
  `Graph.instantiate` raises `UnknownTransportError` instead. Don't "fix" that by
  importing `dfg.transport` into the blueprint layer.
- **Ties break in exactly one place.** `Ordering.pick` is `min(ready, key=self.key)`
  on the base class, and every key's last element is the qualified node ID. A test
  asserts that for every built-in ordering. **Do not reimplement `pick` per
  ordering** — an ordering that forgets the rule would not fail loudly, it would make
  two replays of the same recording legitimately differ.
- **Latency never reads `Message.timestamp`.** A message's timestamp is the *sample*
  time; the latency the control plane reports is wall-clock time on an edge, measured
  against `ControlPlane`'s own injected clock via the private `Envelope.enqueued_ns`.
  An `Envelope` never reaches a node, which is what makes the mistake unavailable
  rather than merely discouraged. `test_control.py` injects timestamps hours apart and
  asserts latency stays in the tens of nanoseconds.
- **`teardown` order is the exact reverse of `setup` order**, and the `setup`-raises
  path is specific: the raising node gets **no** `teardown`, every node already set up
  does, and a node that never ran `setup` gets neither.
- **`block` is not an overflow policy.** It is the obvious fourth one and it deadlocks
  a single-threaded scheduler instantly. It becomes meaningful the first time a
  scheduler is concurrent, and not before.
- **One writer per input port.** Fan-in is rejected at validation, because two
  producers sharing a queue would order messages by which node the scheduler happened
  to fire. Authors use `core.merge` with one port per producer. Fan-*out* from one
  output is fine and is still one topic.
- **Nodes have no sources.** A node with zero input ports is a validation error;
  graph inputs are the only way data enters, which is most of what makes a recorded
  sequence fully determine a run.
- **The declared node form is not deprecated.** `INPUTS`/`OUTPUTS`/`PARAMS` plus
  `run(self, inputs)` is what a node uses when a signature cannot express its ports:
  parameter-dependent ports, `Registry.register_factory`, and port names that are
  runtime data. Deleting it would delete the contract's own escape hatch. The
  framework calls `Node.invoke`, which is the one place the two forms meet — nothing
  wraps or replaces the `run` an author wrote.
- **A port's `type_tag` is a string, not a Python type.** The annotation on a typed
  port says what the payload is; `Port(type_tag=...)` says what the wire is. A tag
  has to survive JSON and mean the same thing to a port in another language, so
  deriving one from the other is not available.

## Naming

Node IDs contain no `.`; a qualified ID is the `.`-joined path of enclosing subgraph
IDs plus the node's own; a topic is `<qualified ID>.<output port>`, so one flat dotted
path. A topic and a qualified node ID are spelled alike and told apart by context — a
topic always ends in a port name. `__`-prefixed port names are the framework's
(`__error__`). A graph output name is an **alias**, not a separate topic: subscribing
to `fusion.pose` and to `fusion.update.fused` observes the same port.

## Commands

```sh
cd dfg/python
pixi run test      # the whole suite (stdlib unittest, ~470 tests)
pixi run demos     # every demo script end to end
pixi run type      # pyrefly over dfg, examples, tests
pixi run imu       # the README's example graph: namespacing, aliases, $param
pixi run schedule  # readiness x ordering, both axes
pixi run replay    # a recording replayed nine ways, one digest
```

`type` is an area-level task rather than part of the root `pixi run type-py`, because
the root environment has no numpy or pyarrow and would report nothing but
missing-import errors for `examples/`. `rust_python_bindings` sets the same
precedent.

**This is the only area with a `pyrefly.toml`**, and having one is the point: with no
config pyrefly falls back to its `basic` preset, which is lenient enough to miss
`x: int = "s"`. The config opts into `default`, which is what makes the typed node
form worth writing — a misspelled parameter, a port name that is not declared, and a
payload type that does not match the port it is wired to are all errors. Its
`search-path` mirrors how the suite runs, so `import dfg` and `import helpers` resolve
the same way `python -m unittest discover -s tests` makes them resolve.

A suppression is written `# pyrefly: ignore[error-kind]` and every one in this area
carries a comment saying why. Most are in tests where **the type error is the
assertion** — writing to a frozen `Message`, passing a non-`Node` to `register`,
naming a reserved port. Two are in `dfg/node.py`, at the seams where the typed form is
built by machinery a checker cannot follow.

The suite must also pass with numpy and pyarrow **absent** — `tests/optional.py` plus
class-level `skipUnless` handles that, and it is the situation a future minimal or
embedded port will be in. Check it with a bare interpreter:
`python3 -m unittest discover -s tests` (expect ~68 skips).

Formatting is repo-wide (`pixi run fmt` from the root, also a `Stop` hook); linting is
`pixi run lint-py`. There is no area-level task for either.

## Layout

```
python/dfg/         the core, stdlib only
  blueprint layer:  blueprint validate flatten serialize mermaid ports readiness errors
  runtime layer:    graph scheduler transport control
  shared:           node registry ordering message
python/examples/    demo scripts; the only place numpy/pyarrow may be imported
  nodes/            core (stdlib) imu (dataclasses) audio video (numpy) arrow (pyarrow)
python/tests/       stdlib unittest, one module per core module plus the examples
```

## Adding things

**A node type**: subclass `Node` and register it. **A type name is serialized into a
blueprint, so it is API**: keep it short and stable. Whichever form you write, a port
carries **zero or more** messages, never exactly one — returning a bare `Message` is
an error, deliberately, because a convenience form would breed nodes that only work
one way.

Prefer the **typed form**, which names parameters and ports as Python identifiers:

```python
class Decimate(Node):
    factor: int = 2                              # a parameter; no default = REQUIRED

    class Out(NamedTuple):
        output: Emit[Any]                        # an output port, in field order

    def __post_init__(self) -> None: ...         # parameter validation goes here

    def run(self, *, input: In[Any] = ()) -> Out:    # an input port; default ()
        return self.Out(output=tuple(kept))
```

`__init_subclass__` derives `PARAMS`/`INPUTS`/`OUTPUTS` from those when the class is
created, so validation still answers from the class and never constructs a node. Two
rules worth knowing: a class-level annotation is a **parameter** unless it is
`ClassVar` (that is what makes `video.ToGray.WEIGHTS` a constant), and a port's
`type_tag` goes in `Annotated[In[T], Port("ImuSample")]` — the annotation types the
payload, the tag types the wire, and they are not interchangeable.

The **declared form** — `INPUTS`/`OUTPUTS`/`PARAMS` as class attributes and
`run(self, inputs)` returning a mapping — is **not deprecated**, and the two are
chosen per *axis*, so a node can mix them. Reach for it when a signature cannot say
what you mean: parameter-dependent ports via the `input_ports`/`output_ports`
classmethods, `Registry.register_factory` (no class to read), or a node whose ports
are runtime data. `tests/helpers.py` keeps several fixtures on it on purpose and each
says why.

**A readiness rule**: subclass `ReadinessRule` and implement `is_ready`, plus `take` if
a firing should consume more than one message per port. Give it a `KIND` and
`register_readiness` it, or blueprints using it cannot round-trip — `PredicateRule` is
the escape hatch that works in memory and refuses to serialize, on purpose.

**An example**: stdlib nodes go in `examples/nodes/core.py`; numpy and pyarrow stay
under `examples/`. Every demo exposes `build_registry()`, `build_blueprint()`, and
`main()` so a test can drive it in process instead of scraping its output, and
everything it processes is synthetic and seeded — no codecs, no downloads, no plotting.

## The contract's demo checklist, and where each one lives

`README.md`'s Python section names what the first implementation must demonstrate.
Each is asserted by a test, not only printed by a script, so `pixi run test` is the
gate.

| Required | Script | Test |
|---|---|---|
| Node lifecycle, including the `setup`-raises path | `lifecycle_demo.py` | `test_lifecycle.py`, `test_examples_imu.py` |
| A nested subgraph with the namespacing | `imu_pipeline.py` | `test_flatten.py`, `test_examples_imu.py` |
| Blueprint round-trip through serialization and a registry | `blueprint_roundtrip.py` | `test_serialize.py` |
| Mermaid rendering from a blueprint | `render_mermaid.py` | `test_mermaid.py` (golden string) |
| At least two points in readiness × ordering | `scheduling_demo.py` (four) | `test_scheduler.py` |
| A recorded-input replay producing identical output twice | `replay_demo.py` | `test_determinism.py` |

Beyond the checklist: `audio_pipeline.py` (numpy, many-outputs-per-firing,
backpressure), `video_pipeline.py` (numpy frames, decimation, and the 200 Hz/30 fps
alignment done by a node — the framework does not align time), and `arrow_batch.py`
(pyarrow, the same blueprint at four chunk sizes producing identical aggregates, which
is the one-engine bet checked as far as it can be).

## Known gaps

Deliberate, and listed in `README.md` under Non-goals: cycles, free-running source
nodes, live topology edits, distributed execution, time-windowed firing rules. Beyond
those, the Python port has not implemented a non-memory transport (the registry is
there and `make_transport` is the seam) and the scheduler is single-threaded only.
