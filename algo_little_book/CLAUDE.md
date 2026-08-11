# algo_little_book

Algorithm and data-structure exercises in Python, each a pixi workspace in its own
directory (e.g. `stack_and_queue/`).

These are exploratory scripts, not unittest problems, so the tasks are named per script rather than `test`/`main`.
`stack_and_queue` exposes:

```sh
cd algo_little_book/stack_and_queue
pixi run stack     # runs stack.py
pixi run queue     # runs queue.py
```

Check a project's `pixi.toml` `[tasks]` for what it actually defines before
assuming `test`/`main` exist.
