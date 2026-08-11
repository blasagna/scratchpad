"""Two structural invariants, enforced instead of documented.

**The core imports nothing but the standard library.** The contract's Tensions
section bets that the portable core is a smaller thing than the framework, and this
is what keeps that bet honest. Walking the imports is *stronger* than splitting the
pixi environment: it catches a stray ``import numpy`` in the core even in an
environment where numpy is installed, which is the environment the examples need.

**The blueprint layer never imports the runtime layer.** "The blueprint is separate
from the instance ... The two layers never merge" is otherwise a sentence in a
document that nothing checks.
"""

import ast
import sys
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parent.parent / "dfg"

BLUEPRINT_LAYER = frozenset(
    {
        "blueprint",
        "errors",
        "flatten",
        "mermaid",
        "message",
        "ports",
        "readiness",
        "serialize",
        "validate",
    }
)
"""Describes, validates, serializes, and draws a graph. Holds no runtime state."""

RUNTIME_LAYER = frozenset({"control", "graph", "scheduler", "transport"})
"""Instantiates a blueprint and processes data. Holds all of the live state."""


def core_modules() -> list[Path]:
    return sorted(CORE.rglob("*.py"))


def imported_roots(path: Path) -> set[tuple[str, str]]:
    """Every ``(root module, full module)`` pair a file imports."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    found: set[tuple[str, str]] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                found.add((alias.name.split(".")[0], alias.name))
        elif isinstance(node, ast.ImportFrom):
            if node.level:  # a relative import; always within dfg
                found.add(("dfg", "dfg"))
            elif node.module:
                found.add((node.module.split(".")[0], node.module))
    return found


class TestCoreIsStdlibOnly(unittest.TestCase):
    def test_there_are_core_modules_to_check(self):
        # A guard that silently checks nothing is worse than no guard.
        self.assertGreater(len(core_modules()), 10)

    def test_every_core_import_is_stdlib_or_dfg_itself(self):
        allowed = set(sys.stdlib_module_names) | {"dfg"}
        offenders: list[str] = []
        for path in core_modules():
            for root, full in sorted(imported_roots(path)):
                if root not in allowed:
                    offenders.append(f"{path.name} imports {full!r}")
        self.assertEqual(
            offenders,
            [],
            "the core must import only the standard library; numpy, pyarrow, and "
            "anything else belong under examples/",
        )

    def test_numpy_and_pyarrow_are_available_but_unused_by_the_core(self):
        # Proves the check has teeth: they *are* installed in this environment, so
        # a stray import would work at run time and only this test would object.
        try:
            import numpy  # noqa: F401
            import pyarrow  # noqa: F401
        except ImportError:
            self.skipTest("numpy/pyarrow not installed in this environment")
        sources = "\n".join(path.read_text(encoding="utf-8") for path in core_modules())
        self.assertNotIn("import numpy", sources)
        self.assertNotIn("import pyarrow", sources)


class TestLayering(unittest.TestCase):
    def test_the_two_layers_name_every_core_module(self):
        names = {path.stem for path in core_modules()} - {"__init__"}
        self.assertEqual(
            names,
            BLUEPRINT_LAYER | RUNTIME_LAYER | {"node", "ordering", "registry"},
            "a new core module must be placed in a layer here, or explicitly "
            "listed as shared",
        )

    def test_the_blueprint_layer_does_not_import_the_runtime_layer(self):
        offenders: list[str] = []
        for path in core_modules():
            if path.stem not in BLUEPRINT_LAYER:
                continue
            for _, full in sorted(imported_roots(path)):
                if full.startswith("dfg.") and full.split(".")[1] in RUNTIME_LAYER:
                    offenders.append(f"{path.name} imports {full}")
        self.assertEqual(
            offenders,
            [],
            "the blueprint layer must stay usable with no runtime: it is what "
            "survives if the one-engine bet turns out wrong",
        )

    def test_a_blueprint_can_be_built_validated_and_drawn_without_the_runtime(self):
        # The property the import rule exists to protect, exercised end to end.
        import helpers
        from dfg.mermaid import render_mermaid
        from dfg.serialize import dumps, loads
        from dfg.validate import validate

        spec = helpers.readme_example_spec()
        registry = helpers.build_registry()
        validate(spec, registry)
        self.assertEqual(loads(dumps(spec)), spec)
        self.assertIn("flowchart", render_mermaid(spec))

    def test_the_runtime_layer_may_import_the_blueprint_layer(self):
        # The dependency is meant to point one way, not to be absent.
        found = imported_roots(CORE / "graph.py")
        self.assertIn(("dfg", "dfg.blueprint"), found)
        self.assertIn(("dfg", "dfg.flatten"), found)


if __name__ == "__main__":
    unittest.main()
