"""Rendering a blueprint as a mermaid diagram.

Run with ``pixi run mermaid``.

The output goes in a ```` ```mermaid ```` block. Compare it with the hand-drawn
diagram in ``dfg/README.md`` -- it should be the same picture, which is the point:
the document's diagram and the blueprint cannot drift apart if one is generated from
the other.

Rendering happens at the **blueprint** layer, so subgraphs stay nested and nothing
has been instantiated. The last section proves that by drawing a blueprint that does
not even validate: renderable before runnable.
"""

from __future__ import annotations

from dfg.blueprint import GraphBuilder
from dfg.mermaid import render_mermaid
from dfg.validate import check
from examples.imu_pipeline import build_blueprint, build_registry, fusion_blueprint
from examples.nodes import imu


def fenced(diagram: str) -> str:
    """Wrap a diagram in a markdown fence, ready to paste."""
    return f"```mermaid\n{diagram}\n```"


def main() -> None:
    spec = build_blueprint()

    print("The tracker blueprint")
    print("=" * 74)
    print(fenced(render_mermaid(spec)))
    print()
    print("Note that `fusion` is drawn as a box with `predict` and `update` inside,")
    print("and the edge labels are topics: `fusion.predict.state` shows that the")
    print("subgraph ID namespaced the node. The edge out of the subgraph is labelled")
    print("with its alias, `fusion.pose`, because that is what the blueprint")
    print("declares -- it resolves to `fusion.update.fused` only once flattened.")
    print()

    print("Top to bottom, without topic labels")
    print("=" * 74)
    print(fenced(render_mermaid(spec, direction="TB", show_topics=False)))
    print()

    print("The fusion subgraph on its own")
    print("=" * 74)
    print("A graph has inputs, outputs, and parameters, so it draws like any other.")
    print(fenced(render_mermaid(fusion_blueprint())))
    print()

    print("A blueprint that does not validate still renders")
    print("=" * 74)
    broken = GraphBuilder("broken")
    broken.add("real", imu.Calibrate)
    broken.add("imaginary", "not.registered")
    broken.connect("real.corrected", "ghost.raw")
    broken.add_input("source", "real.raw")
    problems = check(broken.build(), build_registry())
    print(f"Validation finds {len(problems)} problem(s):")
    for problem in problems:
        print(f"  {problem}")
    print()
    print(fenced(render_mermaid(broken.build())))
    print()
    print("A diagram is something you can get from a description that is still")
    print("wrong, which is when you most want to look at one.")


if __name__ == "__main__":
    main()
