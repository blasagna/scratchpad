# tiny_http_server (Rust)

A very small HTTP server: it binds a socket, then accepts one connection at a time,
reads the request header block, answers `GET` and `HEAD` of `/` with a hello world page,
and logs every event to stderr. See the top-level
[`tiny_http_server/README.md`](../README.md) for the full contract, which the
[C port](../c/README.md) defines and this one is measured against by
[`check_parity.sh`](../check_parity.sh).

## Design

The same two modules and a thin CLI as `c/` and `cpp/`:

- **`http`** — the protocol. Everything in it is either pure or generic over `Read` /
  `Write`, so the whole request-to-response transaction is exercised with `&[u8]` and
  `Vec<u8>` and no socket anywhere.
- **`server`** — the socket layer and the `--file` loader.

Bytes, never `String`: a request line is a stranger's bytes and may hold a NUL or invalid
UTF-8, and both have to survive far enough to be *refused* rather than being truncated or
turned down by a decoder nobody asked to judge them. Same rule, same reason, as
`mini_shell`'s `Vec<u8>` lines.

The seam argument from [`c/README.md`](../c/README.md#there-is-no-seam-here-deliberately)
carries over unchanged: `connect()` completes in the kernel via the accept queue whether
or not `accept` has been called, so `tests/socket.rs` drives a real socket with no thread,
no fork, and no sleep — and a fake socket layer would only have tested the fake.

What follows is what this port does *differently*, and almost all of it is subtraction.

## Four things the standard library already did

| The other two ports hand-write | Here |
|---|---|
| `signal(SIGPIPE, SIG_IGN)` in `main`, plus its failure path | The Rust runtime sets that disposition before `main` runs |
| A `ReadErrorProbe` (C++) or a snapshot of `errno` after `fgetc` (C) | `io::Error` on the `Err` arm |
| `setsockopt(SO_REUSEADDR)` before the `bind` | `TcpListener::bind` sets it |
| `inet_pton` behind a hand-written `->check()` validator | `Ipv4Addr: FromStr` |

**The probe is the interesting one.** A `std::streambuf` has exactly one way to say that no
more bytes are coming — `underflow()` returns `eof()` — so the C++ port has to carry the
failing `recv`'s `std::error_code` out of the socket layer and hand `read_request` a
`std::function` to read it with, or every receive timeout is logged as an ordinary
hang-up. Here the three cases are three arms of one `match`:

```rust
Ok(0)                                    => ConnectionError::Closed,
Err(WouldBlock) | Err(TimedOut)          => ConnectionError::Timeout,
Err(other)                               => ConnectionError::Read(other),
```

and the stream seam stays two ordinary generic parameters. `Interrupted` gets a fourth
arm and is retried: a bare `read(2)` surfaces `EINTR` where stdio restarts the read under
`SA_RESTART`, the same arm `mini_shell/rust`'s `read_line` needs.

**`--host` is the one that retires a rule.** The root [`CLAUDE.md`](../../CLAUDE.md) calls
a hand-written validator the last resort, for a rule no built-in can state, and `--host`
is the C++ port's example of it — `CLI::ValidIPV4` splits on `.` and range-checks four
numbers of its own parsing rather than asking the resolver. Rust's `Ipv4Addr` parser *is*
that grammar: it rejects `localhost`, `127.1`, `0177.0.0.1`, `010.0.0.1`, `1.2.3.04`, and
surrounding whitespace, and accepts `0.0.0.0` and `255.255.255.255`, which is exactly what
`inet_pton` does for every one of them. So this port declares the field as `Ipv4Addr` and
writes no validator at all.

## No `dup`, no `Fd`, no cleanup table

C needs a file-positioning call between a read and a following write on one stream and a
socket has none — `fseek` returns `ESPIPE` — so it `dup`s the accepted descriptor, opens
`"r"` on one and `"w"` on the other, and carries a three-row cleanup table for the ways
`fdopen` can fail while owning one descriptor and not the other. The C++ port answers that
with a hand-written `SocketStreambuf` and an `Fd` type, the repo's first RAII descriptor
owner.

Here `&TcpStream` is both `Read` and `Write`, so `accept_once` is two borrows of one
socket:

```rust
let mut input = BufReader::new(&stream);
let mut output = BufWriter::new(&stream);
serve_connection(&mut input, &mut output, log, opts.page)
```

`TcpStream` owns its descriptor, so there is no `Fd` and no `errno`-preserving `close`.
The buffered reader holding bytes past the blank line was a feature in both other ports
and still is: those are a request body nobody reads, and dropping the reader discards them
for free.

## Two types where the other ports have one

C and C++ share one `Stage` / `HttpResult` enum across the whole package, with a comment
saying which members are fatal. Here that split is in the type system:
[`ConnectionError`] is everything a peer can do and `run` never sees one, and
[`ServerError`] is the server's own failure and ends it. A client cannot end the server,
and there is no variant it could reach to try.

`Response.body` is a `Cow<'_, [u8]>`, which is what retires the `std::string &scratch`
out-parameter the C++ `error_response` takes: a 200 borrows the page — which may be a
1 MiB `--file` and must not be copied per request — and an error owns the few bytes it
just rendered. `route` stays pure *and* infallible with no caller-supplied buffer, which
is why there is still no 500 anywhere.

## `os_message`, which is parity infrastructure

Rust displays an OS error as `No such file or directory (os error 2)`; C's `strerror` and
C++'s `ec.message()` both stop at the message. Two of the log lines `check_parity.sh`
diffs directly are built from one — a missing `--file`, and `--port 80` as a non-root
user — so `os_message` reconstructs the ` (os error N)` suffix from `raw_os_error` and
strips it.

`mini_shell` hit the same wall and answered it by writing its two reachable messages
(`command not found`, `permission denied`) out by hand. That does not scale here: every
errno `bind` and `open` can produce is reachable, so the suffix is removed rather than the
messages replaced.

## `--port` is `u16`, and that is the whole validator

The root [`CLAUDE.md`](../../CLAUDE.md) rule is that a library check carries an option's
grammar wherever it can, and here the type alone does it. `u16`'s range is exactly
0–65535, it reads base 10, it takes a leading `+`, and it turns down `abc`, `0x1F90`,
`8_080`, `-1`, and `65536`. No `value_parser!(u16).range(..)`, and certainly no
hand-written validator — that is the mistake
[`text_analyzer`](../text_analyzer/README.md#known-divergence-argument-parsers) recorded.

It lands closer to C than the C++ port does: `08080` is eight thousand and eighty here and
in C, where CLI11 reads a leading zero as octal and rejects it outright for having an `8`
in it. The one place it is stricter than both is surrounding whitespace. The whole table
is in the [divergences](../README.md#known-divergences).

## Build & run

```sh
cargo test -p tiny_http_server
cargo run  -p tiny_http_server -- --port 8080

target/debug/tiny_http_server --port 8080
curl -i http://127.0.0.1:8080/
```

`cargo run` executes from the workspace root rather than your shell's cwd, so a relative
`--file` path resolves somewhere surprising — the same trap `bazel run` sets for the other
two ports, and exactly why the default page is compiled into the binary.

Exit codes are `0` and `1` as the contract says. A usage error is clap's to report and it
happens to exit `2` as well, which is a coincidence rather than something to rely on; see
the [divergences](../README.md#known-divergences).

Out of memory **aborts** rather than reporting `out of memory` and exiting 1: Rust's
allocator aborts before anything here could see the failure, which is the same call
`mini_shell/rust` and `matrix_ops/rust` already record.

## Checked by hand

`cargo test` reaches more of this port than either other suite reaches of its own —
`tests/cli.rs` spawns the binary and drives a whole `--port 0 --once` run over a real
socket, which the C and C++ READMEs both list here instead. What is left needs a clock or
a browser:

```sh
SRV=target/debug/tiny_http_server

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
python3 -c 'import socket, struct
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()'
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/   # still 200
```

The last one is worth running even though this port installs no handler: it is what says
the runtime's disposition is still in place, and it logs `error writing the response` and
carries on rather than dying silently.

And a real browser at `http://127.0.0.1:8080`, which is the only thing that exercises
preconnect sockets, the unprompted `/favicon.ico` request, and navigating away
mid-response all at once.
