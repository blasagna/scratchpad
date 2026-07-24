# c_little_book

C programming exercises from the little book of C, built with Bazel. Each
exercise is its own Bazel package with a `BUILD` file: `hello_world`,
`arrays_strings`, `recursion`, `struct_enum_union`.

```sh
bazel run  //c_little_book/hello_world:hello
bazel test //c_little_book/recursion:test_math
bazel test //c_little_book/...        # everything under this area
```

Build strictness, the C-tests-with-GoogleTest wrapping (`extern "C"` +
`copts = ["-x", "c++"]`), and the BUILD rule cheat-sheet are repo-wide — see the
root [`CLAUDE.md`](../CLAUDE.md).

`recursion/` has a [`README.md`](recursion/README.md) with exercise notes.
