# tiny_http_server (C)

A very small HTTP server: it binds a socket, then accepts one connection at a time,
reads the request header block, answers `GET` and `HEAD` of `/` with a hello world page,
and logs every event to stderr. See the top-level
[`tiny_http_server/README.md`](../README.md) for the full contract this port defines and
that [`cpp/`](../cpp/README.md) is checked against by
[`check_parity.sh`](../check_parity.sh), with `rust/` still to follow.

## Design

The package is two libraries and a thin CLI, the same shape as `matrix_ops/c`:

- **`http`** — the protocol. Everything in it is either pure or takes a `FILE *`, so the
  whole request-to-response transaction is exercised without a socket anywhere.
- **`server`** — the socket layer and the `--file` loader. It includes `http.h` and
  reuses `HttpResult`, the way `matrix_io.h` reuses `MatrixResult`.

### There is no seam here, deliberately

`mini_shell` put the `fork` behind a runner function pointer because `fork` + `exec`
cannot be driven from a test at all. `simple_logger` put its clock behind an env-var fake
because a clock has no arguments to vary. **Neither argument applies to a socket**, and
the reason is one specific fact: `connect()` to a listening socket completes in the
kernel via the accept queue **whether or not `accept` has been called**. So a
single-threaded test with no fork, no thread, and no sleep can do the whole thing:

```c
server_listen(&opts, &l);            /* --port 0, so the kernel picks a free port */
client = connect_to(l);              /* completes without an accept              */
write(client, "GET / HTTP/1.1\r\n\r\n", n);
server_accept_once(&l, &opts, log);  /* accepts, serves, closes                   */
read_all(client);                    /* assert the exact response bytes           */
```

A fake socket layer would have been strictly worse than that, because it would have
tested the fake. **Take a seam where the real thing is untestable, not merely where it is
impure.**

What the package has instead is granularity. `http_serve_connection(in, out, log, page)`
is the entire per-connection transaction over two streams, so `fmemopen` drives all of
it; `server_accept_once` is the only thing above it, and it is the only thing a socket is
needed for.

| Layer | Purity | Reachable without a socket |
|---|---|---|
| `http_parse_request`, `http_route`, `http_error_response`, `http_sanitize`, `http_status_reason`, `http_result_str` | pure | yes |
| `http_read_request`, `http_write_response`, `http_serve_connection` | `FILE *` only | yes, via `fmemopen` |
| `server_load_page` | opens a file | yes, under `TEST_TMPDIR` |
| `server_listen`, `server_accept_once` | sockets | the `RealSocket` suite only |
| `server_run` | the loop | no — see the bottom of this file |

`main` owns the two process-global acts, `signal(SIGPIPE, SIG_IGN)` and loading `--file`,
for the same reason `mini_shell`'s `setvbuf` lives in `main`: a library function takes
streams its caller owns and does not reach into process state.

## Reading a request, and why EOF is never the answer

`http_read_request` stops at the blank line ending the header block and **never at end of
input**. That is not a stylistic choice: a client holds the connection open after
sending, so anything that reads to EOF — `fread` of a large count, `getdelim` — blocks
until the receive timeout fires. The terminator rule is **the last three bytes being
`\n\r\n`, or the last two being `\n\n`**, which is what accepts all four spellings of a
blank line while not firing on the CRLF that ends the request line itself.

The length is tracked explicitly rather than implied by a NUL, so a NUL inside the
request survives to the parser and is **refused** there. `fgets` was the obvious
alternative and is wrong for exactly this reason: `GET / HTTP/1.1\0junk\r\n\r\n` would
`strlen` down to a valid request line and get a `200`. Same class of bug, and the same
answer, as `mini_shell`'s "a line containing a NUL is refused, not truncated".

Reading is buffered, which is the **mirror image** of `mini_shell`. There, `stdin` had to
be unbuffered so a forked child inherited the bytes the shell had not consumed. Nothing
is forked here, so full buffering is free and correct — and the stdio buffer holding
bytes past the terminator is a feature, since those are a request body nobody reads and
`fclose` discards for free.

## Two streams per socket, and the close sequence

`fdopen(fd, "r+")` is not an option. C requires a file-positioning call between a read and
a following write on the same stream, and a socket has none — `fseek` returns `ESPIPE` —
so the transition is undefined and shows up as dropped or duplicated response bytes
depending on the libc. `server_accept_once` `dup`s the accepted descriptor and opens
`"r"` on one and `"w"` on the other.

**`fdopen` takes ownership only on success**, and that is what the error paths turn on:

| Failure | Cleanup |
|---|---|
| `dup` | `close(fd)` |
| `fdopen(fd, "r")` | `close(fd); close(fd2);` — ownership did not transfer |
| `fdopen(fd2, "w")` | `fclose(in); close(fd2);` and **never** `close(fd)` |

Getting the last one backwards is a double close, which once other descriptors are in
play silently closes something unrelated; getting it backwards the other way leaks a
descriptor and the server dies of `EMFILE` after a few thousand requests.

The connection is then closed with a **lingering close**:

```c
shutdown(fd, SHUT_WR);        /* FIN now, so the client stops waiting */
drain(fd, flags, drain_max);  /* MSG_DONTWAIT and 64 KiB, normally    */
fclose(out);                  /* closes fd2                           */
fclose(in);                   /* closes fd — the socket dies here     */
```

Linux sends an **RST rather than a FIN** when a socket is closed with unread inbound
data, and a peer is allowed to throw away data it already received when it gets an RST.
That is reproducible: `curl -d x http://localhost:8080/` sends a body this server
deliberately never reads, so a bare `close` makes curl print `Recv failure: Connection
reset by peer` instead of showing the `405` that really was sent.

The drain is **non-blocking** on purpose. A blocking one would sit there until the peer
closed, which costs the accept loop a round trip on every connection and lets a client
that reads its response but keeps the socket open stall the whole server for the timeout.
What is left is a narrow race — a body arriving between the drain and the close still
provokes an RST — and on a server that handles one connection at a time that is the right
trade.

**One connection drains blocking, up to 1 MiB**: an oversized header block, the `431`.
Everywhere else the unread bytes are a body the client finished sending long ago, so
whatever has already arrived is all there is; there the server stopped reading at 8 KiB
with the rest of a much larger request still on its way, and it is also the only case
where the bytes at risk are a response the client is owed. `http_serve_connection` reports
it through `left_unread` rather than through its result, because a `431` is a response and
that connection is `HTTP_OK` like any other. `SO_RCVTIMEO` bounds the wait exactly as it
bounds every other read, so the worst case is one connection holding the loop for the
timeout — the same worst case as any slow client.

## The socket, and what is fatal

`socket` → `SO_REUSEADDR` → `bind` → `listen(16)` → `getsockname`, in that order.

`SO_REUSEADDR` has to come before the `bind` and is what makes Ctrl-C-and-rerun work: the
connections just served hold the local port in `TIME_WAIT` for about a minute otherwise.
It is **not** `SO_REUSEPORT` — a second live listener is still refused, so `EADDRINUSE`
keeps meaning "it is already running" rather than silently splitting traffic between two
servers. `RealSocket.RefusesASecondListenerOnTheSamePort` pins that difference.

A backlog of 16: not 1, because a browser loading one page opens a speculative connection
and a favicon connection alongside the page's and this server holds each for a whole
transaction, so a queue of one produces visible refusals; not `SOMAXCONN`, because a deep
queue in front of a server that drains it one at a time only converts a refusal — which a
browser retries at once — into a timeout, which it does not.

`getsockname` runs unconditionally rather than only for `--port 0`, so there is one code
path and the logged port is always the one really in use. The client address is rendered
with `inet_ntop`, never `getnameinfo`: without `NI_NUMERICHOST` that one does a reverse
DNS lookup, which on a loop serving one connection at a time blocks every other client on
a network round trip and can hang for seconds against a broken resolver.

**A client cannot end the server.** Everything a peer does is a per-connection result
that gets logged and moved past. The one exception is `accept`, where a failure is fatal
unless `errno` is `ECONNABORTED` or `EPROTO`. Logging and continuing on every `accept`
failure is the tempting alternative and turns `EMFILE` or `EBADF` into an unkillable loop
spinning at 100% CPU writing the same line forever.

That exception is `accept` and nothing else. The three calls that set an accepted
connection up — `set_timeouts`, `dup`, `fdopen` — fail against *that connection*, not
against the listening socket, so they return `HTTP_ERR_CONNECTION` and the loop moves on;
reporting them as `HTTP_ERR_ACCEPT` would let a one-off `ENOMEM` on one connection end a
server that is still perfectly able to serve. It also keeps the `errno` `server_run`
judges honest: `HTTP_ERR_ACCEPT` is returned by the statement right after the failing
`accept`, with nothing in between, because even a successful `fprintf` may set `errno` —
and a fatal `EMFILE` misread as a transient `ECONNABORTED` is precisely the 100% CPU spin
above.

## `--file`, read once

`server_load_page` runs in `main` before the socket exists, so an unreadable page is a
startup failure with a message and exit `1` rather than a `500` that turns up later
depending on which path somebody visits. That is also what keeps `http_route` pure and
infallible — **there is no `500` anywhere in this server** — and what lets
`Content-Length` come from bytes already in hand with no stat/read race. The cost is that
the page cannot change while the server runs.

It `fstat`s and requires `S_ISREG`, because **`fopen` on a directory succeeds on Linux**
and only fails inside `fread` with `EISDIR`, which would be reported as a read error for
what is really "that is a directory". The same check turns down `/dev/zero` and FIFOs
before they churn through the 1 MiB cap. Reading is chunked into a `realloc`'d buffer
rather than sized by `fseek`/`ftell`, which lies for special files.

Because the page is loaded once and served at a fixed path, **no request byte ever
reaches the filesystem** — there is no path traversal here, not because it is defended
against but because there is no path resolution to attack. That is also why the parser
does no percent-decoding.

## Build & run

```sh
bazel run  //tiny_http_server/c:tiny_http_server
bazel test //tiny_http_server/c:all

bazel-bin/tiny_http_server/c/tiny_http_server --port 8080
curl -i http://127.0.0.1:8080/
```

`bazel run` executes from Bazel's runfiles directory rather than your shell's, so a
relative `--file` path resolves somewhere surprising. Run the binary directly when that
matters — and note that this is exactly why the default page is compiled in rather than
being a `data` dependency.

`--once` stops after a connection that was **answered**, not after any connection at all.
A browser preconnects — it completes a handshake and then sends nothing — so stopping on
the first connection exits the server having served nothing, and the request the user
actually made is then refused. A connection that times out or hangs up without sending
does not count; the `431` does, and so does a response the client left before reading.

The program exits `0` when `--once` served its request or the loop ended cleanly,
**whatever status any request was answered with**; `1` for its own failure (`socket`,
`bind`, `listen`, a non-transient `accept`, an unreadable `--file`); and `2` for a usage
error. Ctrl-C is none of those: there is no handler, so the shell reports `130` and no
closing log line is written.

## Checked by hand

`server_run` has no unit test and cannot have one in this shape: a client has to be
connected before the loop accepts it, and `server_run` does its own binding, so nothing
single-threaded can be waiting on the queue by the time it starts. `SIGPIPE` is likewise
invisible to the suite, since the test binary does not run `main`. Both are checked from
the shell:

```sh
SRV=bazel-bin/tiny_http_server/c/tiny_http_server

# --once serves one connection and exits 0; --port 0 logs the real port
$SRV --port 0 --once

# SO_REUSEADDR: serve a connection, exit, and rebind the same port immediately
$SRV --port 18081 --once & curl -s -o /dev/null http://127.0.0.1:18081/; wait
$SRV --port 18081 --once & curl -s -o /dev/null http://127.0.0.1:18081/; wait

# the drain: shows the 405, not "Recv failure: Connection reset by peer"
$SRV --port 8080 & curl -si -d x http://127.0.0.1:8080/

# --once survives a browser's silent preconnect and still serves the real request
$SRV --port 18082 --once &
python3 -c 'import socket, time
p = socket.create_connection(("127.0.0.1", 18082)); time.sleep(6); p.close()'
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:18082/   # 200, then exit 0

# the blocking drain: a header block far over 8 KiB still gets its 431
python3 -c 'import socket
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"a" * 200000 + b"\r\n\r\n")
print(s.recv(4096).split(b"\r\n")[0])'

# SIGPIPE: hang up before reading, then check the server is still answering
python3 -c 'import socket
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01" + b"\x00" * 7)
s.close()'
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/   # still 200

# startup failures
$SRV --file /nonexistent   # exit 1, "No such file or directory"
$SRV --file /tmp           # exit 1, "not a regular file"
$SRV --port 80             # exit 1, "cannot bind the listening socket: Permission denied"
$SRV --port abc            # exit 2
```

And a real browser at `http://127.0.0.1:8080`, which is the only thing that exercises
preconnect sockets, the unprompted `/favicon.ico` request, and navigating away
mid-response all at once.
