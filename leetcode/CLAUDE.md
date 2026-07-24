# leetcode

LeetCode solutions in Python. Each problem is an independent pixi workspace in its
own directory, holding `solution.py`, `test_solution.py`, a `pixi.toml`, and a
`README.md` describing the problem.

Work from within the problem directory:

```sh
cd leetcode/array_shuffle
pixi run test     # runs the problem's unittest tests
pixi run main     # runs solution.py
```

New problems follow the shared pixi pattern documented in the root
[`CLAUDE.md`](../CLAUDE.md) (`solution.py` + `test_solution.py` + a `pixi.toml`
with `test` and `main` tasks; `unittest` for tests).

The `test` task always runs the problem's unittest suite, but the exact command
string varies per `pixi.toml` — most are `python -m unittest test_solution.py`,
while a few invoke the file directly (`array_set_mismatch`:
`python test_solution.py`) or point at a differently named file (`array_concat`:
`python test_solutions.py`, plural). Check the project's `pixi.toml` `[tasks]`
rather than assuming the command form.
