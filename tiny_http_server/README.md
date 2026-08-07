# tiny http server

An exercise from the little book of c.

Create a very simple http server which responds to web requests. Once running, the user should be able to open a browser at `http://localhost:8080` and see a hello world message served by the program.

## Requirements

1. open a network socket on port 8080
1. wait for a client, including a browser, to connect
1. read the incoming request
1. send a simple http code 200 response with the contents of a hello world html file by default
1. send an error code on invalid url paths
1. close the connection
1. log in the server events in binding the socket, connection state changes, requests received, responses sent

## Contract

The program is implemented in `c/` and `cpp/` (both Bazel) and `rust/` (cargo), and this
section is what all three are checked against. The C port is the **reference
dialect**: where a later port's library gives it different behavior for free, the
difference is recorded under [Known divergences](#known-divergences) rather than
hand-rolled back into agreement. [`check_parity.sh`](check_parity.sh) is what enforces
the rest — it starts each port on an ephemeral port, drives it over a real socket, and
diffs the response bytes, the log, and the exit status.

```
tiny_http_server [options]
```

There are no operands. The server binds a socket, logs that it did, and then repeats:
accept one connection, read its request header block, answer it, close, accept the next.

**Connections are served one at a time**, and that is a deliberate limit rather than an
oversight. There is no `fork`, no thread pool, and no event loop, so there is no reaping,
no shared state, and nothing between the reader and every seam in the package. The cost
is real and is stated rather than hidden: **any one slow client stalls the next**, which
is what the receive timeout bounds and what the loopback default keeps off the network.
A browser that opens a speculative connection and sends nothing on it can delay the page
it is loading by the full timeout.

**A client cannot end the server.** Everything a peer does — hanging up, sending
garbage, sending nothing, sending eight kilobytes of headers — is a per-connection event
that is logged and moved past. Only the listening socket's own failures end the loop.

### Options

| Option | Default | Meaning |
|---|---|---|
| `-p, --port <n>` | `8080` | Port to listen on. `0` asks the kernel for a free one, which is then reported in the `listening on` line. |
| `--host <addr>` | `127.0.0.1` | IPv4 address to bind. A dotted quad only — names are not resolved. |
| `--file <path>` | the built-in page | Serve this file's bytes instead of the compiled-in page. |
| `--once` | off | Serve exactly one request, then exit `0`. A connection that never sends one — a browser's silent preconnect — does not count, or `--once` against a browser would exit having served nothing. |
| `-h, --help` | — | Show help. |

`--port`'s grammar is `strtol` with base 10 fixed, and it is the reference dialect. It
accepts `8080`, `0`, `+8080`, `" 8080"` (`strtol` skips leading whitespace), and `08080`
— which is eight thousand and eighty, not an octal anything. It rejects `""`, `abc`,
`"8080 "`, `0x1F90`, `-1`, `65536`, and anything that overflows (`strtol` saturates at
`LONG_MAX`, which the range check already turns down, so there is no separate `ERANGE`
branch). Ports 1–1023 are **accepted** and then fail at `bind` with `EACCES` for a
non-root user: naming what is a port is the parser's job, and deciding who may have one
is the kernel's.

`--host` is checked with `inet_pton`, not `getaddrinfo`. Resolving names would put DNS —
a blocking network lookup — into the startup of a program that binds exactly one socket,
and would hand back a list of candidates to choose between. So `localhost` is rejected,
with a message saying what was wanted. `inet_pton` also turns down `127.1` and
`0177.0.0.1`, which the older `inet_aton` would have read as `127.0.0.1`. This is the one
grammar every port agrees on byte for byte, and the Rust port gets it without a validator
of its own: `Ipv4Addr`'s parser accepts and rejects exactly what `inet_pton` does.

**The default binds loopback only.** The exercise says to open a browser at
`localhost:8080`, which loopback satisfies completely. This server has no
authentication, no rate limiting past an 8 KiB header cap, and a loop any one slow
client stalls, so `--host 0.0.0.0` puts a trivially wedgeable service on every interface
— worth being a deliberate keystroke.

### Reporting

Every event goes to **stderr**, one unadorned line each, prefixed with the program name:

```
tiny_http_server: listening on 127.0.0.1:8080
tiny_http_server: connection from 127.0.0.1:54012
tiny_http_server: request GET / HTTP/1.1
tiny_http_server: response 200 OK (178 bytes)
tiny_http_server: connection closed
```

No timestamps. A timestamp needs a clock, which means either non-deterministic output or
a fake-clock seam — `simple_logger`'s answer — in a program that otherwise has no clock
at all, and the ports would then have to agree on a format as well as on the events.

**The request line is sanitized before it is logged.** Those are a stranger's bytes on
somebody's terminal: a request line containing `\x1b[2J` would clear the screen of
whoever is watching the server, and one containing a newline would forge a second log
line. Every byte outside printable ASCII becomes `?`, and a line longer than 256 bytes
is truncated with `...` so a prefix is never shown as the whole. Writing the raw bytes is
the obvious thing to write and is the bug.

The byte count on the `response` line is what went **on the wire**, so a `HEAD` reports
`(0 bytes)` even though its `Content-Length` says `178`. The log records what happened;
the header records what the resource is.

Log writes are best effort and unchecked. The log is stderr, and a server that exits
because somebody closed its stderr is worse than one that keeps answering requests
nobody is recording.

### Responses

`HTTP/1.1` in the status line, with `Connection: close` on every response. The header
order is fixed — status line, `Server`, `Content-Type`, `Content-Length`, `Connection`,
then `Allow` where there is one — so the bytes are golden and one assertion covers all
of them.

```
HTTP/1.1 200 OK\r\n
Server: tiny_http_server\r\n
Content-Type: text/html; charset=utf-8\r\n
Content-Length: 178\r\n
Connection: close\r\n
\r\n
<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>tiny_http_server</title></head>
<body><h1>Hello, world!</h1><p>Served by tiny_http_server.</p></body>
</html>
```

| Status | Reason | Body | When |
|---|---|---|---|
| `200` | OK | the page (178 bytes built in) | `GET` or `HEAD` of `/` or `/index.html` |
| `400` | Bad Request | 145 bytes | the request line could not be parsed |
| `404` | Not Found | 141 bytes | any other path |
| `405` | Method Not Allowed | 159 bytes, plus `Allow: GET, HEAD` | any other method |
| `431` | Request Header Fields Too Large | 185 bytes | the header block hit 8 KiB with no blank line |
| `505` | HTTP Version Not Supported | 175 bytes | a major version other than `1` |

**The order of those checks is the contract**: malformed, then version, then method, then
path. A version outranks a method because a method belongs to a protocol — which is what
makes an HTTP/2 preface (`PRI * HTTP/2.0`) a `505` and not a `405`.

**`Content-Length` is not optional.** It is what tells the client the body ended. Letting
the close delimit it — which HTTP/1.0 allowed — makes a truncated response byte-identical
to a complete one, so a client cannot tell the page from half the page and a crash.
`Connection: close` is not optional either: HTTP/1.1 makes persistent connections the
default, and a browser that believed that would hold the socket open awaiting a second
response, stalling every other client on a server that serves one at a time.

**Errors carry a real HTML body.** An empty 404 renders as a blank page — or gets
replaced by the browser's own error page, which looks exactly like a failure to connect
— and shows nothing at all under plain `curl`.

**`HEAD` sends the `Content-Length` the `GET` would have.** Setting the length to `0`
for a `HEAD` is the natural-looking shortcut and it lies about the resource's size, which
is the one thing the method exists to report.

### Edge cases

| Case | Behavior |
|---|---|
| Lone `LF` line endings | Accepted. The header block ends at `\n\r\n` or `\n\n`, so all four spellings of a blank line work. `nc`, `telnet`, and hand-written scripts all send lone LFs, and refusing them makes the first thing anyone tries at a terminal fail. |
| Request line with no terminator | `400`. A target cut off midway is a different target, so it is not parsed as if it had ended. |
| Two-field request line (`GET /`) | `400`. That is HTTP/0.9, which this server does not speak. |
| Four-field request line (`GET /a b HTTP/1.1`) | `400`. A space in a target has to be percent-encoded. |
| One leading empty line | Skipped, per RFC 7230 §3.5 — a client that ends its previous request with a stray CRLF is common enough that refusing it costs more. Exactly one, so a block of nothing but blank lines is still a `400`. |
| NUL byte in the request line | `400`, refused rather than truncated. Everything downstream reads the line as text, so the alternative is letting `GET / HTTP/1.1\0junk` pass as an ordinary request. |
| Header block over 8 KiB with no blank line | `431`. The one read failure whose client is still there to be answered. |
| A 4000-byte target | Parses fine and becomes an ordinary `404`. There is no `414`. |
| `/index.html` | The same page as `/`. |
| A query string (`/?a=1`) | Ignored for routing; the path is the target up to the first `?`. |
| Percent-encoded path (`/%69ndex.html`) | **Not decoded**, so it is a `404`. Nothing here reaches the filesystem, so there is no path to normalize. |
| `/favicon.ico` | An ordinary `404`. Every browser asks for it unprompted, so this is the log's most common line and it is intentional. |
| Absolute-form or asterisk-form target (`GET http://host/`, `PRI *`) | Parses — a server is required to accept them — and routing gives a `404`, or a `505` when the version is not ours. |
| Lowercase `get` | `405`. Methods are case-sensitive, and correcting one would answer a request the client did not send. |
| `HTTP/1.9` | Served. Only the major version is looked at. |
| `HTTP/2.0`, `HTTP/0.9` | `505`. |
| `HTTP/1`, `http/1.1`, `HTTP/11.1` | `400`, not `505`. None of them says which version was meant, so there is nothing for routing to judge. |
| A client that connects and sends nothing | Dropped after the receive timeout (5s), logged as such, **no response**. This is what a browser's speculative connection looks like and it is ordinary — but on a sequential server it is also a stall, which is the whole reason the timeout exists. |
| A client that hangs up mid-response | Logged as a write failure; the loop continues. Requires `SIGPIPE` to be ignored, or the process dies silently. |
| A request body (`curl -d x`) | Never read. The connection is closed with a `shutdown` and a bounded non-blocking drain so the client sees a FIN rather than an RST — an RST lets a peer discard the response it already received, so a bare `close` makes `curl -d x` report a connection reset instead of showing the `405`. |
| A header block far over 8 KiB | `431`, and that one connection drains **blocking** (up to 1 MiB, bounded by the receive timeout) before closing. Everywhere else the unread bytes are a body the client finished sending; here the rest of the request is still on its way, and closing on top of it risks an RST taking the `431` with it. |
| `--file` that is empty | Served, `Content-Length: 0`. A blank page is what was asked for. |
| `--file` that is a directory | Startup error, exit `1`. `fopen` on a directory *succeeds* on Linux and only fails inside `fread` with `EISDIR`, so this is caught with an `fstat` and `S_ISREG` rather than reported as a read error. |
| `--file` over 1 MiB | Startup error, exit `1`. Refused rather than truncated: half a page served as a whole one is worse than a refusal that says why. |
| `--file` that is not HTML | Served as `text/html; charset=utf-8` anyway, so a PNG renders as garbage. Sniffing a type needs a MIME table, which invites a document root, which invites percent-decoding and traversal defenses — a different exercise. |
| `--file` edited while running | No effect. The page is read once at startup; restart to change it. |
| `--port 0` | The kernel picks a free port, reported in the `listening on` line. |
| `--port 80` as a non-root user | Accepted by the parser, then `bind` fails `EACCES` and the server exits `1`. |
| Restarting immediately after serving | Works. `SO_REUSEADDR` is set before the `bind`, so the `TIME_WAIT` remnants of connections just served do not hold the port. |
| A second server on the same port | `EADDRINUSE`, exit `1`. `SO_REUSEADDR` is **not** `SO_REUSEPORT`: a second live listener is still refused, so that error keeps meaning "it is already running". |
| Ctrl-C | Kills the process outright; **no closing log line**, by design. |

### Known simplifications

Each of these is a deliberate omission with the alternative named, not an unfinished
edge:

- **No `Date` header.** The RFC says SHOULD. It needs a clock, which means either
  non-deterministic response bytes or a fake-clock seam in a program that has no other
  clock. No browser and no `curl` requires it.
- **No `Host` enforcement.** HTTP/1.1 says a request without `Host` MUST get a `400`.
  Enforcing it means scanning the header block for a rule no browser and no `curl` can
  trip, and it would make `printf 'GET / HTTP/1.1\r\n\r\n' | nc localhost 8080` — the
  first thing anyone tries by hand — return `400`. Only the request line is parsed at
  all; the header lines after it are ignored entirely.
- **No percent-decoding, and no path traversal to defend against.** The page is loaded at
  startup and served at a fixed path, so **no request byte ever reaches the filesystem**.
  There is no traversal here not because it is defended but because there is no path
  resolution to attack.
- **No request bodies.** Reading stops at the blank line. The lingering close is what
  keeps that from breaking `curl -d`.
- **No keep-alive.** One request per connection, always.
- **No `414` and no `500`.** A long target is a `404` like any other unknown path, and
  there is nothing left that can fail at request time: routing is pure, and the page was
  read before the socket existed.
- **IPv4 only.** `localhost` resolves to `::1` first on most systems, so a browser's
  first `connect` gets `ECONNREFUSED` and falls back to `127.0.0.1` — every current
  browser and `curl` do. Dual-stack means `AF_INET6` with `IPV6_V6ONLY=0`, and then every
  IPv4 client logs as `::ffff:127.0.0.1`.

### The parity harness

[`check_parity.sh`](check_parity.sh) builds all three ports, drives each with the same
bytes over a real socket, and diffs the results against the C port. Nothing is mocked:
every case starts a real server on an ephemeral port and opens a real TCP connection to
it. What gets compared depends on who reports the outcome:

| case kind | compared |
|---|---|
| `serve_case` | the response bytes, the log, and the exit status. The default, and where the contract lives — the log is part of the contract, so it is diffed too. |
| `startup_case` | the log and the exit status, for a server that never gets as far as listening (an unreadable `--file`). |
| `parser_error_case` | stdout only, plus "every port must fail". The parsers are `getopt_long`, CLI11, and clap, and only the rejection is shared; see [Known divergences](#known-divergences). |
| `help_case` | the exit status only. The help text is each parser's own. |

**The only normalization is the port number.** Every port binds `--port 0`, so a case
never collides with a server left running in another terminal, and the kernel's choice
shows up in `listening on 127.0.0.1:<port>` and the client's source port in `connection
from 127.0.0.1:<port>`. Both become `:PORT` before the diff; nothing else is touched, and
the response bytes are compared raw.

**Every case must be one the server answers**, because `--once` stops after an *answered*
request — a case the server does not answer would leave it running until the script's
timeout. The deliberately unanswered ones (a client that sends nothing, one that hangs up
mid-header) live in the unit suites instead. The client is a few lines of Python rather
than `curl`, which would normalize a malformed request line before it reached the socket,
and it half-closes after sending so the drain sees end of input rather than the timeout.

**Two checks are absolute rather than comparisons**, because every port could regress
together and every diff would still be clean: `check_drain` requires the `405` to survive
a request with an unread body, and `check_ephemeral_port` requires `--port 0` to report a
port it really bound. Builds are unoptimized; there are no timings here.

### Known divergences

Everything above is shared by every port, and `check_parity.sh` diffs all of it: **every
response byte and every log line agrees.** What is listed here is what is left, which is
each port's argument parser plus, for Rust, two things its standard library does not let
it say.

**`--port`'s grammar.** C uses `strtol` with base 10 fixed, as spelled out under
[Options](#options). C++ binds the option to an `int` and checks it with
`CLI::Range(0, 65535)` and Rust binds it to a `u16`, both per the root
[`CLAUDE.md`](../CLAUDE.md) rule that a library check carries an option's grammar
wherever it can; hand-writing a validator to match C instead is the mistake
[`text_analyzer`](../text_analyzer/README.md#known-divergence-argument-parsers)
documented. CLI11 converts with base 0, strips group separators, and trims surrounding
whitespace; `u16::from_str` does none of the three:

| `--port` | C | C++ | Rust |
|---|---|---|---|
| `8080`, `+8080` | 8080 | 8080 | 8080 |
| `" 8080"` | 8080 | 8080 | usage error |
| `"8080 "` | usage error | 8080 | usage error |
| `0x1F90`, `8_080` | usage error | 8080 | usage error |
| `02000` | 2000 | **1024** — a leading zero is octal there and decimal here | 2000 |
| `08080` | 8080 | usage error — invalid octal | 8080 |
| `""`, `abc`, `-1`, `65536` | usage error | usage error | usage error |

The `02000` row is the one to watch: it is the only spelling every port accepts and acts
on differently, so it fails quietly rather than as an error. Every other row is somebody
refusing a command line the others take.

**`--host` does not diverge at all**, which is worth stating because it is the only
option grammar that does not. All three accept exactly what `inet_pton` accepts — C and
C++ by calling it, Rust because `Ipv4Addr`'s parser is the same grammar down to rejecting
`010.0.0.1` for its leading zero.

**Each parser's own diagnostics and exit code.** `getopt_long` reports an unknown option,
a bad value, or a stray operand itself at exit `2`. CLI11 brings its own wording and its
own codes — `105` for a `--port` or `--host` it rejects, `109` for an unknown option or a
stray operand, `114` for a repeated one. clap brings its own wording and lands on `2` as
well, which is a coincidence and not a contract. `--help` exits `0` in all three. That is
the same call `simple_logger`, `matrix_ops`, and `mini_shell` made, and it is not worth
reconciling; `check_parity.sh` covers these cases by requiring only that every port
rejects the same command line.

`getopt_long` also matches unambiguous long-option prefixes, so `--onc` works in C and is
an error in the other two.

**A repeated option.** `getopt_long` hands each occurrence to the loop in turn and the
last one wins, because each `case` just overwrites what the previous stored; clap's
default action does the same. CLI11's default multi-option policy rejects the command
line instead, so here it is the C++ port standing alone:

| command | C | C++ | Rust |
|---|---|---|---|
| `--port 9090 --port 0` | binds port 0 | exit `114`, `--port: At most 1 required but received 2` | binds port 0 |
| `--host 127.0.0.1 --host 9.9.9.9` | binds 9.9.9.9 | exit `114` | binds 9.9.9.9 |
| `--file a --file b` | loads `b` | exit `114` | loads `b` |
| `--once --once` | on | on — a flag is exempt from the policy | on |

This is the one divergence `check_parity.sh` cannot be extended to cover in its existing
shape: `parser_error_case` asserts that *every* port rejects the command line, and here
two of them succeed and then serve. Catching it would mean a case that runs a server to
completion under two ports and expects a usage error from the third, which is a different
kind of case than the file has.

**Two things Rust's standard library will not say.** `TcpListener::bind` is `socket`,
`SO_REUSEADDR`, `bind`, and `listen` in a single call returning one `io::Error`, so the
Rust port cannot report which of them failed and calls every one `cannot bind the
listening socket`. That is the right label for every failure reachable without exhausting
file descriptors — `EACCES` on a privileged port, `EADDRINUSE` when the server is already
running — and the wrong one for an `EMFILE` out of `socket`, which no case here reaches.
The same call fixes the backlog at std's 128 rather than the 16 the other two pass, and
that one *is* observable: with the server busy on a connection, C and C++ complete 17
further handshakes before a client stalls and Rust completes 129. That is exactly the
trade `kBacklog` picks 16 for — a refusal a browser retries immediately, rather than a
wait it does not — made the other way. There is no fixing it from here: `TcpListener::bind`
hard-codes the 128 and this crate takes no `libc` or `socket2` dependency to call `listen`
itself.

**Out of memory.** C returns `HTTP_ERR_NOMEM` and C++ catches `std::bad_alloc`, both
reporting `out of memory` and exiting `1`. Rust's allocator aborts before anything in the
port could see the failure, so that one exits with a signal and no log line. The same
call, already recorded, as [`mini_shell`](../mini_shell/README.md) and `matrix_ops`.

Nothing else diverges. In particular the C++ port's single bidirectional stream and the
Rust port's two borrows of one `TcpStream` — where C needs a `dup` and two `FILE *` —
change no observable byte, and neither does a `std::error_code` or an `io::Error`
replacing a global `errno`. Those are in [`cpp/README.md`](cpp/README.md) and
[`rust/README.md`](rust/README.md).

### Exit codes

| Code | When |
|---|---|
| `0` | `--once` answered its request and the server stopped cleanly — **regardless of what status it answered with**. A `404` is the server working. |
| `1` | The server's own failure: `socket`, `bind`, `listen`, a non-transient `accept`, an unreadable `--file`, or a failed `SIGPIPE` disposition. **Not** anything a client did — an accepted connection that could not be set up included, since that is one connection's failure and not the listening socket's. |
| `2` | A usage error: an unknown option, an invalid `--port` or `--host`, or any operand — there are none. |

Ctrl-C is not one of these. There is no handler, so `SIGINT` terminates the process and
the shell reports `130`; installing one buys a tidy log line in exchange for a global, an
async-signal-safety discussion, and a `libc` dependency and an `unsafe` block in the Rust
port, which takes only `clap` like every other Rust port here. The same call
[`mini_shell`](../mini_shell/CLAUDE.md) made about `SIGINT`.
