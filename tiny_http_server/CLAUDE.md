# tiny_http_server

A very small HTTP server: it binds a socket, then accepts one connection at a time,
reads the request header block, answers `GET` and `HEAD` of `/` with a hello world page,
and logs every event to stderr. One port exists so far, `c/` (Bazel); **C++ and Rust are
still to come**, and the C port is the reference dialect they will be measured against.
The full contract — the responses, the status precedence, the edge cases, the deliberate
simplifications, and the exit codes — is in [`README.md`](README.md), and the port has
its own design notes in [`c/README.md`](c/README.md).

## Commands

```sh
bazel run  //tiny_http_server/c:tiny_http_server
bazel test //tiny_http_server/c:all

# Run the binary directly rather than through `bazel run` whenever a relative
# --file path matters: `bazel run` executes from the runfiles directory.
bazel-bin/tiny_http_server/c/tiny_http_server --port 8080

curl -i http://127.0.0.1:8080/
printf 'GET / HTTP/1.1\r\n\r\n' | nc 127.0.0.1 8080
```

**There is no `check_parity.sh` yet**, and there should be one as soon as the second port
lands — same shape as `mini_shell`'s and `simple_logger`'s, with the C port as the
reference. It will need `--port 0` (so cases never collide with a server left running in
another terminal) and `--once` (so a case does not have to background the server and kill
it by PID); both options exist for that reason as much as for their own.

**Two things no unit test reaches**, both covered from the shell instead:

- `server_run` itself. A client has to be connected before the loop accepts it, and
  `server_run` does its own binding, so nothing single-threaded can be waiting on the
  queue by the time it starts. Its two decisions — `--once` stopping after one
  connection, and a fatal bind failure — are checked by hand; the list is at the bottom
  of [`c/README.md`](c/README.md).
- `SIGPIPE`. `main` ignores it and the test binary does not run `main`, so the suite
  cannot observe its absence. Reproduce it by connecting, sending a request, and closing
  with `SO_LINGER` set to zero so the peer gets an RST before reading the response.

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
  bug.
- **The log has no timestamps**, so a parity script can diff stderr directly. Adding one
  means a clock, which means either non-deterministic output or importing
  `simple_logger`'s fake-clock seam into a program with no other clock — and then the
  ports have to agree on a format as well as on the events.
- **Log writes are best effort and unchecked.** The log is stderr; a server that exits
  because somebody closed its stderr is worse than one that keeps answering requests
  nobody is recording. This is deliberately unlike `mini_shell`, where a failed write to
  `err` is a `SHELL_ERR_WRITE` that ends the loop.

## Gotchas

- **A socket needs two `FILE *`, not one.** `fdopen(fd, "r+")` requires a file-positioning
  call between a read and a following write, and a socket has none — `fseek` returns
  `ESPIPE` — so the transition is formally undefined and practically drops or duplicates
  bytes depending on the libc. `server_accept_once` therefore `dup`s the accepted
  descriptor and opens `"r"` on one and `"w"` on the other. **`fdopen` takes ownership
  only on success**: when the second one fails the cleanup is `fclose(in)` plus
  `close(fd2)` and *never* `close(fd)`, which would be a double close that later silently
  closes some unrelated descriptor.
- **`SIGPIPE` is ignored in `main`, and the `FILE *` write seam is what forces it.**
  `MSG_NOSIGNAL` is a flag on `send` and `fwrite` has nowhere to put it; `SO_NOSIGPIPE`
  is a BSD option that does not exist on Linux. So the choice is: keep the stream seam
  and ignore the signal process-wide, or take `MSG_NOSIGNAL` and lose `fmemopen` testing
  of the response bytes — the most valuable test target in the package. The seam wins.
  Delete the `signal` call and the server **dies silently, with no message and no log
  line**, the first time a browser navigates away mid-response: SIGPIPE's default action
  is termination. It lives in `main` and not the library for the same reason
  `mini_shell`'s `setvbuf` does, and the test binary needs its own copy.
- **`SO_REUSEADDR` before `bind`, and it is not `SO_REUSEPORT`.** Without it, restarting
  inside about a minute fails `EADDRINUSE` on the `TIME_WAIT` remnants of the connections
  just served — which is every Ctrl-C and rerun during development. It does **not** permit
  a second live listener, so `EADDRINUSE` keeps meaning "the server is already running"
  and stays useful. `SO_REUSEPORT` would allow two servers on 8080 and split connections
  between them at random, which is a memorably horrible afternoon.
  `RealSocket.RefusesASecondListenerOnTheSamePort` is what pins the difference; without
  it, swapping one for the other passes everything else.
- **The connection is closed with `shutdown` plus a non-blocking drain, not a bare
  `close`.** Linux sends an RST rather than a FIN when a socket is closed with unread
  inbound data, and a peer may discard data it already received when it gets an RST.
  Reproduce it with `curl -d x http://localhost:8080/`: the request body is deliberately
  never read, so a bare close makes curl report `Recv failure: Connection reset by peer`
  instead of showing the `405` that really was sent. The drain uses `MSG_DONTWAIT` on
  purpose — **a blocking drain sits there until the peer closes**, which costs the accept
  loop a round trip on every connection and lets a client that reads its response and
  keeps the socket open stall the whole server for the timeout.
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
  forked here, so full buffering is free — and the stdio buffer holding bytes past the
  blank line is desirable, since those are a request body nobody reads and `fclose` throws
  away for free. **Do not copy the `setvbuf` line across.**
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
- **`valgrind` does not run on this machine.** Use sanitizers instead:
  ```sh
  bazel test //tiny_http_server/c:all --config=permissive \
    --copt=-fsanitize=address --copt=-g --linkopt=-fsanitize=address
  ```

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
