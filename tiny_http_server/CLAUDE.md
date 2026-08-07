# tiny_http_server

A very small HTTP server: it binds a socket, then accepts one connection at a time,
reads the request header block, answers `GET` and `HEAD` of `/` with a hello world page,
and logs every event to stderr. Three ports exist: `c/` and `cpp/` (both Bazel) and
`rust/` (cargo workspace member `tiny_http_server`), and the C port is the reference
dialect the others are measured against. The full contract — the responses, the status
precedence, the edge cases, the deliberate simplifications, and the exit codes — is in
[`README.md`](README.md), and each port has its own design notes ([c](c/README.md),
[cpp](cpp/README.md), [rust](rust/README.md)).

## Commands

```sh
bazel run  //tiny_http_server/c:tiny_http_server
bazel run  //tiny_http_server/cpp:tiny_http_server
cargo run  -p tiny_http_server

bazel test //tiny_http_server/c:all
bazel test //tiny_http_server/cpp:all
cargo test -p tiny_http_server

./tiny_http_server/check_parity.sh          # build all three, run 33 cases, diff them
./tiny_http_server/check_parity.sh --keep   # keep the work dir for inspection

# Run the binary directly rather than through `bazel run` or `cargo run` whenever
# a relative --file path matters: those execute from the runfiles directory and
# the workspace root respectively.
bazel-bin/tiny_http_server/c/tiny_http_server --port 8080
target/debug/tiny_http_server --port 8080

curl -i http://127.0.0.1:8080/
printf 'GET / HTTP/1.1\r\n\r\n' | nc 127.0.0.1 8080
```

**`check_parity.sh` compares the response bytes, the log, and the exit status**, and the
whole of the contract agrees today — the only recorded divergences are the argument
parsers' and two things Rust's standard library will not say. Three things about its
shape are load-bearing:

- **It uses `--port 0` and `--once`, which is most of why those options exist.** A fixed
  port collides with the server somebody left running in another terminal, which is
  exactly when they run this; `--once` is what lets a case avoid backgrounding the server
  and killing it by PID.
- **The only normalization is the port number.** `listening on 127.0.0.1:<port>` and the
  client's source port in `connection from 127.0.0.1:<port>` are replaced with `:PORT`.
  Nothing else is touched, and the response bytes are compared raw.
- **Every case must be one the server answers.** `--once` stops after an *answered*
  request, so a case the server does not answer (a client that sends nothing, one that
  hangs up mid-header) would leave the server running until the script's timeout. Those
  live in the unit suites instead.
- **Two checks there are absolute, not comparisons**, because both ports could regress
  together and every diff would still be clean: `check_drain` requires the `405` to
  survive a request with an unread body, and `check_ephemeral_port` requires `--port 0` to
  report a port it really bound.

**Two things no unit test reaches in the Bazel ports**, both covered from the shell
instead:

- `server_run` / `run` itself. A client has to be connected before the loop accepts it,
  and it does its own binding, so nothing single-threaded can be waiting on the queue by
  the time it starts. Its two decisions — `--once` stopping after one answered request,
  and a fatal bind failure — are checked by hand; the lists are at the bottom of
  [`c/README.md`](c/README.md) and [`cpp/README.md`](cpp/README.md). **The Rust port
  reaches both from `cargo test`**, because a Rust integration test can spawn the binary:
  `rust/tests/cli.rs` starts a real `--port 0 --once` server, reads the port out of its
  log, drives it over a socket, and waits for exit 0, and `rust/tests/socket.rs` covers
  the bind failure.
- `SIGPIPE`. `main` ignores it and the test binary does not run `main`, so the suite
  cannot observe its absence. Reproduce it by connecting, sending a request, and closing
  with `SO_LINGER` set to zero so the peer gets an RST before reading the response. The
  Rust port has nothing to delete here — the runtime sets the disposition — but the same
  reproduction is still worth running against it, since an explicit `SIG_DFL` would put
  the bug back.

## Shared behavior (keep the ports in sync)

- **Connections are served one at a time, and that is the design.** No `fork`, no
  threads, no event loop — so no reaping, no fd hygiene in two processes, and no
  concurrency between the reader and every seam in the package. The cost is stated rather
  than hidden: one slow client stalls the next, which the receive timeout bounds and the
  loopback default keeps off the network. **Do not "fix" this in one port**; a port that
  forks is a port whose log interleaves and whose parity cases stop being comparable.
- **A client cannot end the server.** Everything a peer does comes back as a
  per-connection result that is logged and moved past; only the listening socket's own
  failures end the loop. This is the direct analogue of `mini_shell`'s "a failed command
  never ends the loop", and the exit code says the same thing: `0` means the loop ran,
  whatever any request got answered with.
- **The one exception to that is `accept`.** A failure there is fatal unless `errno` is
  `ECONNABORTED` or `EPROTO`, which mean the peer vanished between the handshake and the
  accept. Logging and continuing on every `accept` failure is the tempting alternative
  and it turns `EMFILE` or `EBADF` into an unkillable loop spinning at 100% CPU writing
  the same line forever — far worse than exiting with it.
- **`accept` and nothing else.** Setting an accepted connection up (`set_timeouts`,
  `dup`, `fdopen`) fails against that connection, not against the listening socket, so
  those are `HTTP_ERR_CONNECTION` and the loop moves on; folding them into the accept
  result lets a one-off `ENOMEM` on one connection end the server. It also protects the
  `errno` the fatal-or-not decision reads: the accept result is returned by the statement
  right after the failing `accept`, with **no logging or cleanup in between**, because
  even a successful `fprintf` may set `errno` — and a fatal `EMFILE` misread as a
  transient `ECONNABORTED` is exactly the 100% CPU spin above. Every port needs its own
  spelling of "capture the failure before doing anything else with it". **In C++ that
  spelling makes the rule unbreakable rather than merely documented**: `Result` is
  `{Stage, std::error_code}`, the code is captured into the value at the point of failure,
  and `run` judges `served.ec` rather than a global. A port whose result type carries the
  error has nothing left to get wrong here; one that leans on `errno` has to keep the rule
  in its head. **Rust goes one further and makes the fatal/per-connection split a type
  distinction**: `accept_once` returns `Result<Outcome, ServerError>` and `ServerError` has
  no variant a client can reach, so a setup failure cannot be returned where `run` would
  judge it even by accident. Its transience test is `kind() == ConnectionAborted ||
  raw_os_error() == Some(EPROTO)`, with `EPROTO` spelled as the literal `71` because this
  crate takes no `libc` dependency — the same call `mini_shell`'s tests make for `EAGAIN`.
- **`--once` stops after a request, not after a connection.** Browsers preconnect, so the
  first connection is often a silent one that times out; stopping on it exits having
  served nothing and the real request is refused. A timeout or a hang-up does not count,
  the `431` does, and so does a response the client left before reading.
- **Reading is split from parsing, because EOF never arrives.** A client holds the
  connection open after sending, so anything that reads to end of input — `fread` of a
  large count, `getdelim`, Rust's `read_to_end` — blocks until the timeout.
  `http_read_request` stops at the blank line and nothing else. Every port needs its own
  spelling of that and none of them can lean on a read-to-end.
- **The header block ends at `\n\r\n` or `\n\n`**, which accepts all four spellings of a
  blank line without firing on the CRLF that ends the request line itself. Requiring
  `\r\n\r\n` exactly is the obvious rule and it rejects `nc`, `telnet`, and every
  hand-written script, which all send lone LFs.
- **Only the request line is parsed.** The header lines after it are ignored entirely,
  which is why there is no `Host` check and why a request body is never read. A port that
  starts parsing headers has taken on `Host` enforcement, `Content-Length` handling, and
  chunked bodies along with it.
- **A NUL in the request line is refused, not truncated.** Everything downstream reads
  the line as text, so the alternative is letting `GET / HTTP/1.1\0junk` pass as an
  ordinary request. Length is carried explicitly rather than implied by a NUL for exactly
  this reason — the same trap, and the same answer, as `mini_shell`'s NUL handling.
- **The status precedence is malformed, version, method, path**, and it is contract
  rather than implementation. A version outranks a method because a method belongs to a
  protocol, which is what makes an HTTP/2 preface (`PRI * HTTP/2.0`) a `505` and not a
  `405`. `Route.TheVersionOutranksTheMethod` is what pins it.
- **A well-formed method we do not serve is not a parse failure.** `POST` parses fine and
  becomes a `405` with an `Allow`; rejecting it at parse time would make it a `400` and
  lose the `Allow` that tells the client what would have worked. Methods are
  case-sensitive, so `get` lands there too.
- **`http_route` is pure and infallible, and there is no `500` anywhere.** The page is
  read at startup, so a request never touches the filesystem, and the error bodies are
  rendered into the caller's scratch buffer rather than allocated. Any port that reads
  `--file` per request has just introduced a `500`, a failure path through routing, and a
  stat/read race in which the `Content-Length` and the body disagree.
- **`Content-Length` is mandatory and `HEAD` still sends the real one.** Letting the
  close delimit the body makes a truncated response byte-identical to a complete one.
  Setting the length to `0` for a `HEAD` is the natural-looking shortcut and lies about
  the resource's size, which is the one thing the method exists to report — which is why
  body suppression is a parameter of the writer and not a decision inside the router.
- **The response header order is fixed** (status line, `Server`, `Content-Type`,
  `Content-Length`, `Connection`, then `Allow`), so the bytes are golden and one
  assertion covers all of them. Reordering them in one port silently breaks the shared
  golden.
- **The request line is sanitized before it is logged.** Attacker-controlled bytes going
  to a terminal: `\x1b[2J` clears the screen of whoever is watching, and a newline forges
  a second log line. Every byte outside printable ASCII becomes `?` and a long line is
  truncated with `...`. Logging the raw bytes is the obvious thing to write and is the
  bug. **The limit is 255 characters, not 256**: C spells it as a 256-byte destination
  buffer and spends one byte on the NUL, so a port that has no buffer — and therefore no
  NUL — has to say 255 or its truncated lines are one character longer than everyone
  else's. Only a request line over 255 bytes shows the difference, which is why
  `check_parity.sh` has a 4000-byte-target case.
- **The log has no timestamps**, so a parity script can diff stderr directly. Adding one
  means a clock, which means either non-deterministic output or importing
  `simple_logger`'s fake-clock seam into a program with no other clock — and then the
  ports have to agree on a format as well as on the events.
- **Log writes are best effort and unchecked.** The log is stderr; a server that exits
  because somebody closed its stderr is worse than one that keeps answering requests
  nobody is recording. This is deliberately unlike `mini_shell`, where a failed write to
  `err` is a `SHELL_ERR_WRITE` that ends the loop.

## Gotchas

- **In C, a socket needs two `FILE *`, not one — and in C++ and Rust it needs neither.**
  `fdopen(fd, "r+")` requires a file-positioning call between a read and a following
  write, and a socket has none — `fseek` returns `ESPIPE` — so the transition is formally
  undefined and practically drops or duplicates bytes depending on the libc.
  `server_accept_once` therefore `dup`s the accepted descriptor and opens `"r"` on one and
  `"w"` on the other. **`fdopen` takes ownership only on success**: when the second one
  fails the cleanup is `fclose(in)` plus `close(fd2)` and *never* `close(fd)`, which would
  be a double close that later silently closes some unrelated descriptor. **That rule is
  stdio's and `std::basic_filebuf`'s, not `std::streambuf`'s**: the C++ port writes a
  `SocketStreambuf` with its own get and put areas over one descriptor, passes one
  `SocketStream` as both the `istream &` and the `ostream &`, and has no dup, no second
  stream, and no cleanup table at all. **Rust does not have the rule either**: `&TcpStream`
  is both `Read` and `Write`, so `accept_once` hands `serve_connection` a
  `BufReader::new(&stream)` and a `BufWriter::new(&stream)` — two borrows of one socket —
  and `TcpStream` owns its own descriptor, so there is no `Fd` and no cleanup either. A
  port that transliterates the C dance has taken on the hazard without the constraint
  that caused it.
- **`SIGPIPE` is ignored in `main` in both Bazel ports, but not for the same reason — and
  the Rust port gets it before `main` runs.** In C the
  `FILE *` write seam forces it: `MSG_NOSIGNAL` is a flag on `send`, `fwrite` has nowhere
  to put it, and `SO_NOSIGPIPE` is a BSD option that does not exist on Linux — so the
  choice is keep the stream seam and ignore the signal process-wide, or take
  `MSG_NOSIGNAL` and lose `fmemopen` testing of the response bytes, the most valuable test
  target in the package. The seam wins. **A hand-written streambuf could take
  `MSG_NOSIGNAL` and keep the seam**, so the C++ port is not forced — and still does it,
  because the log goes to `std::cerr` and `tiny_http_server 2>&1 | head` would otherwise
  kill the server on the closed log pipe, which is the exact thing the best-effort log
  rule exists to prevent. One process-wide disposition covers both directions; do not add
  a second mechanism alongside it. Delete the call in either port and the server **dies
  silently, with no message and no log line**, the first time a browser navigates away
  mid-response: SIGPIPE's default action is termination. It lives in `main` and not the
  library for the same reason `mini_shell`'s `setvbuf` does, and each test binary needs
  its own copy. **The Rust port has no call to delete** — the runtime sets `SIG_IGN`
  before `main` — so the only way to reintroduce the bug there is to put a disposition
  back deliberately. Its `main` says so in a comment for exactly that reason; the absence
  of a line is not self-documenting.
- **A `std::streambuf` cannot report "error" — only "no more bytes".** `underflow()`
  returning `eof()` is the whole vocabulary, so a receive timeout and a browser's silent
  preconnect are indistinguishable at the stream level. `SocketStreambuf` therefore keeps
  the failing `recv`'s `std::error_code` and `read_request` takes a `ReadErrorProbe` that
  reads it: empty is `kClosed`, `EAGAIN`/`EWOULDBLOCK` is `kTimeout`, anything else is
  `kRead` — the same three cases the C port gets from `errno` after `fgetc`. Drop the
  probe and every timeout is logged as an ordinary hang-up, which no unit test that hands
  in a string stream would notice. **The Rust port needs no probe at all**: `io::Error`
  carries the reason on the `Err`, so `Ok(0)`, `WouldBlock`/`TimedOut`, and anything else
  are three arms of one `match` in `read_request`, and the stream seam stays two ordinary
  generic parameters. It does need a fourth arm the other two have no counterpart to —
  `Interrupted`, because a bare `read(2)` surfaces `EINTR` where stdio restarts the read
  under `SA_RESTART`, the same arm `mini_shell/rust` needs.
- **`last_error` inside `SocketStreambuf` is not the `errno` helper.** The class has a
  member of that name, so within its member functions `error_ = last_error()` resolves to
  the accessor and assigns `error_` to itself — it compiles, both return
  `std::error_code`, nothing warns, and every receive timeout silently becomes a clean
  close. The file-local helper is called `errno_error` for exactly this reason. This cost
  a debugging session; do not rename it back.
- **`SO_REUSEADDR` before `bind`, and it is not `SO_REUSEPORT`.** Without it, restarting
  inside about a minute fails `EADDRINUSE` on the `TIME_WAIT` remnants of the connections
  just served — which is every Ctrl-C and rerun during development. It does **not** permit
  a second live listener, so `EADDRINUSE` keeps meaning "the server is already running"
  and stays useful. `SO_REUSEPORT` would allow two servers on 8080 and split connections
  between them at random, which is a memorably horrible afternoon.
  `RealSocket.RefusesASecondListenerOnTheSamePort` is what pins the difference, and
  `refuses_a_second_listener_on_the_same_port` in `rust/tests/socket.rs`; without them,
  swapping one for the other passes everything else. **The Rust port sets neither option
  by hand**: `TcpListener::bind` already sets `SO_REUSEADDR` and does not set
  `SO_REUSEPORT`, which is exactly the behavior wanted — but it means the guarantee lives
  in the standard library rather than in a line anybody can grep for, so that test is the
  only thing in the port that says it.
- **The connection is closed with `shutdown` plus a non-blocking drain, not a bare
  `close`.** Linux sends an RST rather than a FIN when a socket is closed with unread
  inbound data, and a peer may discard data it already received when it gets an RST.
  Reproduce it with `curl -d x http://localhost:8080/`: the request body is deliberately
  never read, so a bare close makes curl report `Recv failure: Connection reset by peer`
  instead of showing the `405` that really was sent. The drain uses `MSG_DONTWAIT` on
  purpose — **a blocking drain sits there until the peer closes**, which costs the accept
  loop a round trip on every connection and lets a client that reads its response and
  keeps the socket open stall the whole server for the timeout. **One case is exempt**: a
  header block over `HTTP_REQUEST_MAX`, where the server stopped reading at 8 KiB with the
  rest of a much larger request still arriving, and where the bytes a close would cost are
  a `431` the client is owed rather than a body nobody read. That one drains blocking up
  to 1 MiB, bounded by `SO_RCVTIMEO` like every other read. `http_serve_connection`
  reports it through an out-parameter rather than its result, because a `431` is a
  response and that connection is `HTTP_OK` like any other.
- **Browsers open sockets they never write to.** Chrome and Firefox preconnect: they
  complete a handshake and then send nothing, sometimes for seconds. On a sequential
  accept loop that is a wedged server, which is why `SO_RCVTIMEO` exists and why
  `HTTP_ERR_CLOSED` and `HTTP_ERR_TIMEOUT` are separate results — the first is the
  ordinary case and deserves a calm log line rather than an error. `SO_SNDTIMEO` covers
  the mirror image, which only bites with a large `--file`: a client that stops reading
  blocks the write once the socket's send buffer fills.
- **`/favicon.ico` appears in the log on every page view, and it is a `404`.** Every
  browser asks for it unprompted. `Route.FaviconIsAnOrdinaryNotFound` exists so nobody
  reads those lines as a bug and special-cases the path away — the only real fix would be
  to serve an icon, which this exercise does not have.
- **`localhost` may resolve to `::1` first.** The server is `AF_INET` only, so a browser's
  first `connect` gets `ECONNREFUSED` on IPv6 and falls back to `127.0.0.1`. Every current
  browser and `curl` do fall back, so it works — but it makes `strace` confusing and looks
  like an intermittent failure to anyone watching. `http://127.0.0.1:8080` never has the
  problem.
- **`curl` and a browser exercise different code paths.** `curl` sends one request and
  reads to `Content-Length`, so it never touches the preconnect timeout, the favicon
  route, or the RST-on-close case unless you give it `-d`. A browser exercises all of
  them. Check **both** before believing a change. `curl -X POST`, `curl -I`, `curl -d x`,
  and `printf 'GET / HTTP/1.1\r\n\r\n' | nc 127.0.0.1 8080` cover what a browser will not
  reach.
- **`bazel run` and a relative `--file`.** `bazel run` executes from Bazel's runfiles
  directory rather than your shell's cwd, so `--file page.html` resolves somewhere
  surprising. Pass an absolute path, or run `bazel-bin/tiny_http_server/c/tiny_http_server`
  directly. It is also exactly why the default page is compiled into the binary instead of
  being a `data` dependency. (Same trap as `copy_file`, `simple_logger`, and
  `mini_shell`.)
- **`--port 0` is a feature, not a placeholder.** It asks the kernel for an ephemeral
  port, which `getsockname` then reports in the `listening on` line. It is what makes the
  `RealSocket` suite possible under `bazel test` without a fixed port that collides with
  the server left running in another terminal — which is precisely when you are most
  likely to be running tests. `getsockname` runs unconditionally so there is one code
  path.
- **The socket tests need no thread, no fork, and no sleep**, because `connect()` to a
  listening socket completes in the kernel via the accept queue whether or not `accept`
  has been called. That single fact is why this package has **no seam** where `mini_shell`
  took a runner function pointer: take a seam where the real thing is untestable, not
  merely where it is impure. A fake socket layer would only have tested the fake.
- **The request stream is buffered, which is the exact opposite of `mini_shell`.** There,
  `stdin` had to be `_IONBF` so a forked child inherited unconsumed bytes. Nothing is
  forked here, so full buffering is free — and the buffer holding bytes past the blank
  line is desirable, since those are a request body nobody reads and `fclose` (or dropping
  the streambuf's get area, or dropping the `BufReader`) throws away for free. **Do not
  copy the `setvbuf` line across**, in C++ do not reach for `sync_with_stdio`, and in Rust
  do not reach for the `dup`-of-fd-0-as-a-plain-`File` dance `mini_shell/rust` needs —
  none of the three has anything to do with a socket here.
- **`Fd` in `cpp/server.hpp` is the repo's first RAII descriptor owner**, and it is
  deliberate rather than an oversight elsewhere: `mini_shell/cpp` keeps its pipe in a bare
  `int[2]` because that pipe lives inside one function, while a listener and every
  accepted connection here outlive the function that made them. It is also what makes the
  C port's `fdopen`-ownership table unrepresentable. `reset()` saves and restores `errno`
  around the `close`, the same care `close_quietly` takes in C — a failing `close` would
  otherwise overwrite the error the caller is about to report. The Rust port has no
  counterpart because `TcpStream` and `TcpListener` already are one; do not add a wrapper
  around a raw fd there, which would need `unsafe` to build and would close fd 0 on drop
  if it were ever handed one.
- **`accept4(..., SOCK_CLOEXEC)` sits behind `_GNU_SOURCE`.** Use plain `accept`; there is
  no exec here, so `FD_CLOEXEC` buys nothing. Same trap, same reasoning, as `mini_shell`'s
  `pipe2` note.
- **Nothing in this repo defines a POSIX feature-test macro, and that is load-bearing
  here.** Bazel passes no `-std` for C, so gcc uses `-std=gnu17`, `__STRICT_ANSI__` is
  undefined, and glibc exposes all of POSIX; the test side compiles as `-std=c++20`, where
  glibc exposes POSIX regardless. **If `--copt=-std=c17` is ever added to `.bazelrc`, every
  socket declaration vanishes at once** — the fix is `#define _POSIX_C_SOURCE 200809L` as
  the first line of `server.c` and `test_server.c`, before any `#include`.
- **`-Wsign-compare` is in `-Wextra`**, and the read, drain, and write loops all compare a
  `ssize_t` return against a `size_t` cap. That is the likeliest place `-Werror` stops a
  change to any of them.
- **`fopen` on a directory succeeds.** `--file /tmp` opens fine and only fails inside
  `fread` with `EISDIR`, producing "cannot read the page file" for what is really "that is
  a directory". `server_load_page` therefore `fstat`s and requires `S_ISREG`, which also
  turns down `/dev/zero` and FIFOs before they churn through the whole 1 MiB cap.
- **Rust's `io::Error` prints ` (os error N)` and nothing else does.** `strerror` and
  `std::error_code::message` both stop at the message, and two of the log lines
  `check_parity.sh` diffs directly are built from one — a missing `--file`
  (`tiny_http_server: <path>: No such file or directory`) and `--port 80` as a non-root
  user (`cannot bind the listening socket: Permission denied`). `os_message` in
  `rust/src/server.rs` reconstructs the suffix from `raw_os_error` and strips it, and
  every Rust log line and startup message that names an errno goes through it. Print an
  `io::Error` directly anywhere in that port and the two startup cases fail. This is the
  same wall `mini_shell` hit; it answered by writing its two reachable messages out by
  hand, which does not work here because every errno `bind` and `open` can produce is
  reachable.
- **`TcpListener::bind` is four syscalls, so the Rust port has one fatal stage where the
  others have three.** `socket`, `SO_REUSEADDR`, `bind`, and `listen` all happen inside
  it and it returns one `io::Error`, so every failure is reported as `cannot bind the
  listening socket`. That is right for everything reachable (`EACCES`, `EADDRINUSE`) and
  wrong for an `EMFILE` out of `socket`. Do not try to recover the distinction by
  pre-flighting a socket — it would be a different descriptor than the one bound. It is a
  recorded divergence, and `local_addr()` is still separate, so `cannot listen on the
  socket` survives for that one.
- **`valgrind` needs `libc6-dbg` installed** (see root [`README.md`](../README.md)) or
  it fails at startup instead of running memcheck:
  ```sh
  bazel test //tiny_http_server/... --config=valgrind
  ```
  `--config=asan` is the faster alternative for iterating:
  ```sh
  bazel test //tiny_http_server/... --config=asan
  ```

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
