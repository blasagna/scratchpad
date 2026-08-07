# tiny_http_server (C++)

A very small HTTP server: it binds a socket, then accepts one connection at a time,
reads the request header block, answers `GET` and `HEAD` of `/` with a hello world page,
and logs every event to stderr. See the top-level
[`tiny_http_server/README.md`](../README.md) for the full contract, which the
[C port](../c/README.md) defines and this one is measured against by
[`check_parity.sh`](../check_parity.sh).

## Design

The same two libraries and a thin CLI as `c/`, in one namespace, `http_server`:

- **`http`** — the protocol. Everything in it is either pure or takes a
  `std::istream &` / `std::ostream &`, so the whole request-to-response transaction is
  exercised with `std::istringstream` and `std::ostringstream` and no socket anywhere.
- **`server`** — the socket layer and the `--file` loader.

`Stage` and `Result` are shared between them the way `MatrixResult` is shared in
`matrix_ops/c`, and `describe(Stage)` returns byte-identical strings to the C port's
`http_result_str` because several of them are logged.

The seam argument from [`c/README.md`](../c/README.md#there-is-no-seam-here-deliberately)
carries over unchanged: `connect()` completes in the kernel via the accept queue whether
or not `accept` has been called, so the `RealSocket` suite drives a real socket with no
thread, no fork, and no sleep — and a fake socket layer would only have tested the fake.
What follows is what this port does *differently*, and why.

## One stream, one descriptor

`SocketStreambuf` is the reason there is no `dup` here.

C requires a file-positioning call between a read and a following write on one stream,
and a socket has none — `fseek` returns `ESPIPE` — so the C port opens `"r"` on the
accepted descriptor and `"w"` on a `dup` of it, and carries a three-row cleanup table for
the ways `fdopen` can fail while owning one descriptor and not the other. Getting a row
backwards is either a descriptor leak (`EMFILE` after a few thousand requests) or a
double close that later shuts something unrelated.

**That rule belongs to stdio and to `std::basic_filebuf`, not to `std::streambuf`.** A
buffer written by hand keeps its own get and put areas over one descriptor, and the
transition between reading and writing is nothing at all — no flush, no seek, no second
descriptor. So `accept_once` is:

```cpp
Fd conn{::accept(...)};
SocketStream stream(conn.get());
Transaction tx = serve_connection(stream, stream, log, opts.page, probe);
```

The same object is passed as both the `std::istream &` and the `std::ostream &`.
`serve_connection` never learns they are the same, which is exactly what keeps the tests
able to hand it two different string streams instead. `SocketStream.ReadsThenWritesOnOneDescriptor`
is what pins the claim: read a socketpair to exhaustion, then write to it, on one fd.

The stdio buffer holding bytes past the blank line was a feature in the C port, and still
is: those are a request body nobody reads, and dropping the get area discards them for
free.

## A streambuf cannot say "error", so the probe does

`std::streambuf` has exactly one way to report that no more bytes are coming:
`underflow()` returns `eof()`. There is no channel for *why*, and the difference matters
here — a browser's silent preconnect (`kClosed`) and a receive timeout (`kTimeout`) are
different log lines, and the second is the one the whole `SO_RCVTIMEO` design exists for.

So `SocketStreambuf` keeps the failing `recv`'s `std::error_code` and `SocketStream`
exposes it, and `read_request` takes a `ReadErrorProbe` — a
`std::function<std::error_code()>` supplied by the layer that has a socket:

| probe returns | stage |
|---|---|
| nothing | `kClosed` — the peer closed cleanly |
| `EAGAIN` / `EWOULDBLOCK` | `kTimeout` — `SO_RCVTIMEO` fired |
| anything else | `kRead` |

This is the same shape as `mini_shell`'s `Runner`: inject the impure thing from the layer
that owns it, and let the stream-only tests pass nothing. Those get `kRead` from the
stream's own `badbit`, which is how `mini_shell`'s suite injects a read failure too.

The C port reads the global `errno` for this. It gets away with it because `fgetc`'s
failure is the statement before the check, and it snapshots `errno` before even calling
`ferror`.

## `Result` carries the error, which retires a rule

C's sharpest gotcha in this package is that `HTTP_ERR_ACCEPT` must be returned by the
statement immediately after the failing `accept`, **with no logging or cleanup in
between**, because `server_run` decides whether to end the server by reading the global
`errno` and even a successful `fprintf` may set one. A fatal `EMFILE` misread as a
transient `ECONNABORTED` is an unkillable loop spinning at 100% CPU.

Here `Result` is `{Stage, std::error_code}` and the code is captured into the value at
the point of failure:

```cpp
if (accepted < 0)
  return {Stage::kAccept, errno_error()};
```

`run` then judges `served.ec`, not a global. The rule is not merely easier to follow; it
has nothing left to violate. Every other `Result` in the package works the same way,
which is why `LoadPage.ReportsAMissingFileWithTheErrorIntact` can assert on the error
without arranging for nothing to have run in between.

## `Fd`, and the one new idiom

`Fd` is a move-only descriptor owner, and the first one in this repo — `mini_shell/cpp`
keeps its pipe in a bare `int[2]` with a hand-written close helper. It earns its place
because a listener and every accepted connection outlive the function that made them, and
because it is what makes the C port's cleanup table unrepresentable rather than merely
documented. `reset()` saves and restores `errno` around the `close`, the same care
`close_quietly` takes in the C port.

Everything else follows the house style: free functions in a namespace, `std::optional`
for the one pure parser that can fail, `std::string_view` for non-owning text (so a NUL
in a request line survives to be refused rather than truncating it), plain structs for
`Result` / `Response` / `Options` / `Listener`, and `std::bad_alloc` caught in `main` and
reported as `kNoMem`.

`route` stays pure and infallible, which is why there is no `500`: `error_response`
renders into a `std::string &scratch` the caller owns rather than returning an owned
body. Returning the body by value would have been simpler and would copy a 1 MiB `--file`
page on every 200.

## `--port` is the library's, `--host` is not

The root [`CLAUDE.md`](../../CLAUDE.md) rule is that a library check carries an option's
grammar wherever it can. So `--port` is bound to an `int` and checked with
`CLI::Range(0, 65535)`, and the resulting difference from C's `strtol` is
[recorded as a divergence](../README.md#known-divergences) rather than hand-rolled back
into agreement — hand-writing a validator to match C is precisely the mistake
`text_analyzer` documented.

`--host` is the last-resort case the same rule names: **no built-in states this rule.**
It gets a hand-written `->check()` calling `inet_pton`, the same call `matrix_ops` makes
for its NaN `--scalar`. `CLI::ValidIPV4` is the tempting alternative and is not the same
grammar — it splits on `.` and range-checks four numbers of its own parsing — so taking
it would have moved which addresses this port accepts away from C's for no gain.

## `SIGPIPE` is still ignored, for a second reason

The C port's notes say the choice was: keep the `FILE *` seam and ignore `SIGPIPE`
process-wide, or take `MSG_NOSIGNAL` on the `send` and lose the in-memory testing of the
response bytes. A hand-written streambuf is a stream seam *and* has somewhere to put the
flag, so this port could have had both.

It still calls `std::signal(SIGPIPE, SIG_IGN)`, because `MSG_NOSIGNAL` only covers the
socket. **The log goes to `std::cerr`**, and `tiny_http_server 2>&1 | head` would kill the
server on the closed log pipe — which is exactly what the best-effort log rule exists to
prevent. One process-wide disposition covers both directions; two mechanisms would not
cover more, so there is one.

## Build & run

```sh
bazel test //tiny_http_server/cpp:all
bazel run  //tiny_http_server/cpp:tiny_http_server

bazel-bin/tiny_http_server/cpp/tiny_http_server --port 8080
curl -i http://127.0.0.1:8080/
```

`bazel run` executes from Bazel's runfiles directory rather than your shell's, so a
relative `--file` path resolves somewhere surprising. Run the binary directly when that
matters — and note that this is exactly why the default page is compiled in rather than
being a `data` dependency.

Exit codes are `0` and `1` as the contract says. A usage error is **not** `2` here: CLI11
reports it and brings its own code (`105` for a bad `--port` or `--host`, `109` for an
unknown option or a stray operand), which is a
[recorded divergence](../README.md#known-divergences).

## Checked by hand

`run` has no unit test and cannot have one in this shape: a client has to be connected
before the loop accepts it, and `run` does its own binding, so nothing single-threaded can
be waiting on the queue by the time it starts. `SIGPIPE` is likewise invisible to the
suite, since the test binary does not run `main`. `check_parity.sh` covers most of what
follows against both ports at once; these are what is left.

```sh
SRV=bazel-bin/tiny_http_server/cpp/tiny_http_server

# --once serves one request and exits 0; --port 0 logs the real port
$SRV --port 0 --once

# SO_REUSEADDR: serve a connection, exit, and rebind the same port immediately
$SRV --port 18081 --once & curl -s -o /dev/null http://127.0.0.1:18081/; wait
$SRV --port 18081 --once & curl -s -o /dev/null http://127.0.0.1:18081/; wait

# --once survives a browser's silent preconnect and still serves the real request
$SRV --port 18082 --once &
python3 -c 'import socket, time
p = socket.create_connection(("127.0.0.1", 18082)); time.sleep(6); p.close()'
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:18082/   # 200, then exit 0

# SIGPIPE: hang up before reading, then check the server is still answering
$SRV --port 8080 &
python3 -c 'import socket
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01" + b"\x00" * 7)
s.close()'
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/   # still 200
```

And a real browser at `http://127.0.0.1:8080`, which is the only thing that exercises
preconnect sockets, the unprompted `/favicon.ico` request, and navigating away
mid-response all at once.
