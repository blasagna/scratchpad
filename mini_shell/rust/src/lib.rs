//! A prototype shell: print a prompt, read one command per line, split it into
//! a program and its arguments, run that program, and report the status of
//! anything that did not exit 0.
//!
//! The library half of the port. `main.rs` owns the command line and the real
//! streams; everything here takes its streams and its runner as parameters, so
//! the whole loop is exercised in tests without forking a single process. See
//! `README.md` for the design notes and `mini_shell/README.md` for the contract
//! the C and C++ ports are held to as well.

use std::ffi::OsStr;
use std::fmt;
use std::io::{self, ErrorKind, Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::process::ExitStatusExt;
use std::process::{Command, ExitStatus};

/// Written before every read, and flushed.
pub const PROMPT: &str = "$ ";

/// The name every diagnostic is prefixed with.
pub const PROG_NAME: &str = "mini_shell";

/// The startup banner. Written as a table so the art stays readable in the
/// source, and with `write_all` rather than `write!` so a `{` in a future banner
/// is not a format hole — the same hazard the C port avoids by using `fputs`
/// over `printf`. Every backslash is doubled: these are Rust string literals,
/// and the compiler would otherwise read `\_` as an unknown escape.
const BANNER: [&str; 8] = [
    " __  __  _        _   ____   _            _  _ ",
    "|  \\/  |(_) _ __  (_) / ___| | |__    ___ | || |",
    "| |\\/| || || '_ \\ | | \\___ \\ | '_ \\  / _ \\| || |",
    "| |  | || || | | || |  ___) || | | ||  __/| || |",
    "|_|  |_||_||_| |_||_| |____/ |_| |_| \\___||_||_|",
    "",
    "commands run directly, one program per line; type 'exit' to quit",
    "",
];

/// The word that ends the loop, matched after trimming whitespace.
const EXIT_WORD: &[u8] = b"exit";

/// The whitespace `trim` strips, written out rather than delegated to
/// [`u8::is_ascii_whitespace`]. That method omits `\v`, which C's `isspace` in
/// the "C" locale and the C++ port's `" \t\n\v\f\r"` both include — so
/// `"\x0bexit\x0b"` would end the loop in two ports and run as a command here.
const SPACE: &[u8] = b" \t\n\x0b\x0c\r";

/// What the shell failed at on its own behalf.
///
/// A failing *command* is not one of these: that is a [`Status`], reported per
/// command, and it never ends the loop nor changes the exit code.
///
/// Two variants where the C port's `ShellResult` has four. `getline` returns
/// `-1` for end of input, a read error, and an allocation failure alike, which
/// is the only reason `shell_run` has to consult `ferror`/`feof` and the only
/// reason `SHELL_ERR_NOMEM` exists. [`Read::read`] separates them in the type:
/// `Ok(0)` is end of input and `Err(_)` is a read error. The allocation case
/// never arrives here at all, because allocation failure aborts the process — a
/// deliberate divergence, recorded in `README.md`.
#[derive(Debug)]
pub enum ShellError {
    /// A read error on the command input stream.
    Read(io::Error),
    /// A write error on the output or error stream.
    Write(io::Error),
}

impl ShellError {
    /// The C port's `shell_result_str` labels, minus the ones that have no
    /// variant here.
    pub fn label(&self) -> &'static str {
        match self {
            ShellError::Read(_) => "error reading command input",
            ShellError::Write(_) => "error writing output",
        }
    }
}

impl fmt::Display for ShellError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ShellError::Read(err) | ShellError::Write(err) => write!(f, "{}: {err}", self.label()),
        }
    }
}

impl std::error::Error for ShellError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            ShellError::Read(err) | ShellError::Write(err) => Some(err),
        }
    }
}

/// How one command ended.
///
/// The first two come out of the wait status; the last three are the ways a
/// command can fail to start at all. The C and C++ ports tell those apart by
/// the errno `execvp` left behind, which reaches their parent through a
/// close-on-exec pipe; here they arrive as an [`io::Error`] because
/// [`Command::status`] runs that same pipe internally.
#[derive(Debug)]
pub enum Status {
    /// The command ran and exited with this code.
    Exited(i32),
    /// The command was killed by this signal.
    Signaled(i32),
    /// No such program (`ENOENT`).
    NotFound,
    /// The program exists but could not be executed (`EACCES`).
    NotExecutable,
    /// The command never started for some other reason, carried here rather
    /// than left in a global `errno` for the reporting code to pick up later.
    Unrunnable(io::Error),
}

/// The seam that keeps [`run`] from spawning anything itself.
///
/// The C port spells it as a function pointer plus a `void *` context and the
/// C++ port as a `std::function`; here it is a trait, so the recording fake the
/// tests use is a plain struct whose fields they read afterwards.
///
/// `argv[0]` is the program; the rest are its arguments.
pub trait Runner {
    fn run(&mut self, argv: &[&[u8]]) -> io::Result<ExitStatus>;
}

/// The one impure `Runner`: runs `argv[0]` directly, with `argv[1..]` as its
/// arguments.
///
/// This is `fork` + `execvp` + `waitpid`, which is exactly what the C and C++
/// ports spell out by hand — including the close-on-exec pipe that carries a
/// failed exec's errno back from the child, which [`Command`] runs internally
/// and reports as `Err`.
pub struct ExecRunner;

impl Runner for ExecRunner {
    fn run(&mut self, argv: &[&[u8]]) -> io::Result<ExitStatus> {
        // No arg0(): Command already passes the program as argv[0], which is
        // what execvp(argv[0], argv) does with the word as typed. A program with
        // no '/' is looked up on PATH, as execvp does.
        //
        // Not Strings: the contract passes non-ASCII bytes through unchanged,
        // and neither a program name nor an argument is required to be UTF-8.
        Command::new(OsStr::from_bytes(argv[0]))
            .args(argv[1..].iter().map(|word| OsStr::from_bytes(word)))
            // status(), not output(): the command inherits our stdin, stdout,
            // and stderr, which is what makes `cat` work.
            .status()
    }
}

/// What [`run`] needs beyond its three streams.
pub struct Options<'a> {
    pub show_banner: bool,
    pub runner: &'a mut dyn Runner,
}

/// Reduces what the runner reported to the outcomes worth naming.
///
/// Pure, and separate from [`report_status`], which is what makes the signal
/// case testable without arranging for a real process to be killed.
///
/// The C and C++ ports decode a raw wait status by hand, checking signals
/// before exits so that a command killed by `SIGKILL` is not reported as
/// "exited with status 0". [`ExitStatus`] has already done that decoding; the
/// order below is the same one for the same reason, since a status that names a
/// signal has no exit code to report.
pub fn decode_status(raw: io::Result<ExitStatus>) -> Status {
    let status = match raw {
        Ok(status) => status,
        // The C port's -1: the command never started, so the error is about the
        // shell rather than about the command. Which error it was is the
        // difference between the two messages a user can act on and the
        // catch-all one they cannot.
        Err(err) => {
            return match err.kind() {
                ErrorKind::NotFound => Status::NotFound,
                ErrorKind::PermissionDenied => Status::NotExecutable,
                _ => Status::Unrunnable(err),
            };
        }
    };
    if let Some(signal) = status.signal() {
        return Status::Signaled(signal);
    }
    // A stopped child names neither, and reports as a clean exit, matching what
    // the C port's `if (WIFEXITED(raw))` leaves `code` at.
    Status::Exited(status.code().unwrap_or(0))
}

/// Writes one line about a command that did not succeed. A clean exit is silent.
///
/// The two common ways to fail to start name `program` in mini_shell's own
/// words rather than borrowing [`io::Error`]'s. That is deliberate and
/// load-bearing for cross-port parity: every port writes these bytes itself,
/// where the text of an errno differs between them — Rust's carries an
/// ` (os error N)` suffix the C ports' `strerror` does not.
pub fn report_status<W: Write>(err: &mut W, status: &Status, program: &[u8]) -> io::Result<()> {
    // The program is written as bytes rather than through `{}`: a program name
    // is whatever the user typed, and need not be UTF-8.
    let mut name = |suffix: &str| {
        err.write_all(PROG_NAME.as_bytes())?;
        err.write_all(b": ")?;
        err.write_all(program)?;
        err.write_all(suffix.as_bytes())
    };

    match status {
        Status::Exited(0) => Ok(()),
        Status::Exited(code) => writeln!(err, "{PROG_NAME}: command exited with status {code}"),
        Status::Signaled(signal) => {
            writeln!(err, "{PROG_NAME}: command terminated by signal {signal}")
        }
        Status::NotFound => name(": command not found\n"),
        Status::NotExecutable => name(": permission denied\n"),
        Status::Unrunnable(cause) => writeln!(err, "{PROG_NAME}: failed to run command: {cause}"),
    }
}

/// Writes the startup banner.
pub fn write_banner<W: Write>(out: &mut W) -> io::Result<()> {
    for line in BANNER {
        out.write_all(line.as_bytes())?;
        out.write_all(b"\n")?;
    }
    Ok(())
}

/// Narrows a line to what is left after stripping ASCII whitespace from both
/// ends. Interior bytes, NULs included, are untouched.
pub fn trim(line: &[u8]) -> &[u8] {
    let start = line
        .iter()
        .position(|byte| !SPACE.contains(byte))
        .unwrap_or(line.len());
    let end = line
        .iter()
        .rposition(|byte| !SPACE.contains(byte))
        .map_or(start, |last| last + 1);
    &line[start..end]
}

/// Splits a command line into words on ASCII whitespace, using the same set as
/// [`trim`].
///
/// This is the whole of mini_shell's grammar. There is no quoting, no escaping,
/// and no expansion of any kind: a run of whitespace separates two words and
/// every other byte is literal, so `echo a | wc` runs `echo` with the three
/// arguments `a`, `|`, and `wc`. Splitting is the job `/bin/sh -c` used to do,
/// and taking it back is the point of this port.
///
/// A line that is empty or entirely whitespace yields no words; callers skip
/// such lines before getting here.
pub fn split(line: &[u8]) -> Vec<&[u8]> {
    line.split(|byte| SPACE.contains(byte))
        // A run of whitespace splits into empty slices between the separators.
        // They are not words: passing them on would hand the program blank
        // arguments it never asked for.
        .filter(|word| !word.is_empty())
        .collect()
}

/// Whether this line is the one builtin. `EXIT`, `exitx`, and `exit 3` are not:
/// they are commands like any other and go to the interpreter, which is what a
/// real shell does with them.
pub fn is_exit_command(line: &[u8]) -> bool {
    trim(line) == EXIT_WORD
}

/// Whether this line is empty or whitespace only, and so not worth a fork.
pub fn is_blank(line: &[u8]) -> bool {
    trim(line).is_empty()
}

/// Writes the prompt and flushes it. The command inherits `out`'s file
/// descriptor, so an unflushed prompt would surface after the command's own
/// output.
fn put_prompt<W: Write>(out: &mut W) -> io::Result<()> {
    out.write_all(PROMPT.as_bytes())?;
    out.flush()
}

/// Reads one line, up to and including its `\n`, appending to `line` and
/// returning how many bytes were read. `Ok(0)` is end of input.
///
/// One byte at a time, deliberately. Anything that buffers would read ahead
/// past the command about to run, and a command that reads stdin — `cat`,
/// `read`, `ssh` — must get the input mini_shell has not consumed yet. That is
/// also why `main` hands this a plain `File` over fd 0 rather than
/// `io::stdin()`, which is always a `BufReader`: **reaching for
/// `BufRead::read_until` here puts the read-ahead bug straight back**, and no
/// unit test can catch it, since they all drive [`run`] with an in-memory
/// stream where buffering is invisible.
fn read_line<R: Read>(input: &mut R, line: &mut Vec<u8>) -> io::Result<usize> {
    let mut byte = [0u8; 1];
    let mut read = 0;
    loop {
        match input.read(&mut byte) {
            Ok(0) => return Ok(read),
            Ok(_) => {
                line.push(byte[0]);
                read += 1;
                if byte[0] == b'\n' {
                    return Ok(read);
                }
            }
            // stdio restarts an interrupted read under SA_RESTART; `impl Read
            // for File` is a bare read(2) and surfaces the EINTR, so without
            // this arm a SIGWINCH mid-read would be reported as a read error.
            // write_all and flush already retry internally.
            Err(err) if err.kind() == ErrorKind::Interrupted => continue,
            Err(err) => return Err(err),
        }
    }
}

/// The command loop: prompt, read, run, report, until `exit` or end of input.
///
/// The streams are three parameters rather than three globals, so tests drive
/// this with in-memory buffers. `err` is separate from `out` on purpose: it is
/// the contract that the shell's own complaints never mix into the command's
/// output.
pub fn run<R: Read, O: Write, E: Write>(
    input: &mut R,
    out: &mut O,
    err: &mut E,
    opts: &mut Options<'_>,
) -> Result<(), ShellError> {
    if opts.show_banner {
        write_banner(out).map_err(ShellError::Write)?;
    }

    let mut line = Vec::new();
    loop {
        put_prompt(out).map_err(ShellError::Write)?;

        line.clear();
        if read_line(input, &mut line).map_err(ShellError::Read)? == 0 {
            // End of input: leave the cursor on a fresh line, since the prompt
            // just written is the last thing on this one. The loop ending at
            // `exit` gets no such newline.
            return out
                .write_all(b"\n")
                .and_then(|()| out.flush())
                .map_err(ShellError::Write);
        }

        // Strip one '\n', then one '\r', so CRLF input runs the same commands as
        // LF input. A '\r' anywhere else belongs to the command.
        if line.last() == Some(&b'\n') {
            line.pop();
        }
        if line.last() == Some(&b'\r') {
            line.pop();
        }

        if is_blank(&line) {
            continue;
        }
        if is_exit_command(&line) {
            return Ok(());
        }

        if line.contains(&0) {
            // execvp takes NUL-terminated strings, so running this would run
            // `echo a` and silently drop the rest of `echo a\0rm -rf /`.
            // Refused rather than truncated.
            writeln!(err, "{PROG_NAME}: command contains a NUL byte").map_err(ShellError::Write)?;
            continue;
        }

        // Splitting is mini_shell's whole grammar, and it is done here rather
        // than by an interpreter: the runner is handed a program and its
        // arguments, not a command line. The line is blank-checked above, so
        // there is always at least one word and a program to name in any
        // diagnostic.
        let argv = split(&line);
        let status = decode_status(opts.runner.run(&argv));
        report_status(err, &status, argv[0]).map_err(ShellError::Write)?;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A wait status as the runner returns one. The encoding is not something
    /// the standard library promises, so these build the layout every platform
    /// this runs on uses, and `tests/real_exec.rs` checks that assumption
    /// against a real process.
    fn exited(code: i32) -> ExitStatus {
        ExitStatus::from_raw(code << 8)
    }

    fn signaled(signal: i32) -> ExitStatus {
        ExitStatus::from_raw(signal)
    }

    /// EAGAIN, spelled as its number because this crate takes no `libc`
    /// dependency.
    const EAGAIN: i32 = 11;

    /// A `Runner` that records what it was asked to run and hands back canned
    /// statuses, so the command loop is exercised without forking anything.
    /// Statuses are consumed in order; once they run out, every further command
    /// succeeds.
    #[derive(Default)]
    struct FakeRunner {
        commands: Vec<Vec<Vec<u8>>>,
        statuses: Vec<io::Result<ExitStatus>>,
        next: usize,
    }

    impl Runner for FakeRunner {
        fn run(&mut self, argv: &[&[u8]]) -> io::Result<ExitStatus> {
            self.commands
                .push(argv.iter().map(|word| word.to_vec()).collect());
            let status = match self.statuses.get(self.next) {
                Some(Ok(status)) => Ok(*status),
                // io::Error is not Clone, so a canned failure is rebuilt rather
                // than handed out twice — from its raw errno when it has one,
                // and otherwise from its kind, which is what decode_status
                // matches on.
                Some(Err(err)) => Err(match err.raw_os_error() {
                    Some(code) => io::Error::from_raw_os_error(code),
                    None => io::Error::from(err.kind()),
                }),
                None => Ok(exited(0)),
            };
            self.next += 1;
            status
        }
    }

    /// One command as the runner receives it, spelled the way the assertions
    /// below read best.
    fn argv(words: &[&str]) -> Vec<Vec<u8>> {
        words.iter().map(|word| word.as_bytes().to_vec()).collect()
    }

    /// An input stream that fails rather than yielding anything, standing in for
    /// the write-only `fmemopen` stream the C suite uses.
    struct FailingReader;

    impl Read for FailingReader {
        fn read(&mut self, _buf: &mut [u8]) -> io::Result<usize> {
            Err(io::Error::from_raw_os_error(EAGAIN))
        }
    }

    /// An output stream with no room, standing in for the C suite's one-byte
    /// `fmemopen` buffer.
    struct FailingWriter;

    impl Write for FailingWriter {
        fn write(&mut self, _buf: &[u8]) -> io::Result<usize> {
            Err(io::Error::from_raw_os_error(EAGAIN))
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    /// An input stream that reports one interrupted read before yielding its
    /// bytes.
    struct InterruptedOnceReader<'a> {
        rest: &'a [u8],
        interrupted: bool,
    }

    impl Read for InterruptedOnceReader<'_> {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if !self.interrupted {
                self.interrupted = true;
                return Err(io::Error::from(ErrorKind::Interrupted));
            }
            self.rest.read(buf)
        }
    }

    /// Result of one `run` over a fixed input: everything a caller can observe.
    /// `err` is captured separately from `out` because the two streams are the
    /// contract.
    struct RunOutput {
        result: Result<(), ShellError>,
        out: String,
        err: String,
    }

    fn text(bytes: Vec<u8>) -> String {
        String::from_utf8(bytes).expect("the shell's own output is UTF-8")
    }

    fn drive<R: Read>(input: &mut R, fake: &mut FakeRunner, show_banner: bool) -> RunOutput {
        let mut out = Vec::new();
        let mut err = Vec::new();
        let mut opts = Options {
            show_banner,
            runner: fake,
        };
        let result = run(input, &mut out, &mut err, &mut opts);
        RunOutput {
            result,
            out: text(out),
            err: text(err),
        }
    }

    fn run_shell(input: &[u8], fake: &mut FakeRunner) -> RunOutput {
        drive(&mut { input }, fake, false)
    }

    fn reported(status: &Status) -> String {
        reported_for(status, b"acommand")
    }

    fn reported_for(status: &Status, program: &[u8]) -> String {
        let mut err = Vec::new();
        report_status(&mut err, status, program).expect("writing to a Vec cannot fail");
        text(err)
    }

    // --- decode_status ---

    #[test]
    fn a_clean_exit_decodes_as_exited_zero() {
        assert!(matches!(decode_status(Ok(exited(0))), Status::Exited(0)));
    }

    #[test]
    fn a_nonzero_exit_keeps_its_code() {
        assert!(matches!(decode_status(Ok(exited(3))), Status::Exited(3)));
    }

    #[test]
    fn an_exit_of_127_is_not_special_cased() {
        // It used to be how the interpreter said "command not found". Nothing
        // says that now — a missing program never reaches a wait status at all —
        // so 127 is whatever the command chose to exit with, like any other.
        assert!(matches!(
            decode_status(Ok(exited(127))),
            Status::Exited(127)
        ));
    }

    #[test]
    fn a_signal_is_not_read_as_an_exit_code() {
        assert!(matches!(
            decode_status(Ok(signaled(9))),
            Status::Signaled(9)
        ));
    }

    #[test]
    fn a_missing_program_is_told_apart_by_its_error_kind() {
        let failed = Err(io::Error::from(ErrorKind::NotFound));
        assert!(matches!(decode_status(failed), Status::NotFound));
    }

    #[test]
    fn an_unexecutable_program_is_told_apart_by_its_error_kind() {
        let failed = Err(io::Error::from(ErrorKind::PermissionDenied));
        assert!(matches!(decode_status(failed), Status::NotExecutable));
    }

    #[test]
    fn any_other_error_is_unrunnable_and_is_carried() {
        // A failed fork lands here, and the error is carried rather than left
        // in a global for the reporting to pick up later.
        let failed = Err(io::Error::from_raw_os_error(EAGAIN));
        assert!(matches!(decode_status(failed), Status::Unrunnable(_)));
    }

    // --- report_status ---

    #[test]
    fn a_successful_command_is_reported_silently() {
        assert_eq!(reported(&Status::Exited(0)), "");
    }

    #[test]
    fn report_names_the_exit_status() {
        assert_eq!(
            reported(&Status::Exited(3)),
            "mini_shell: command exited with status 3\n"
        );
    }

    #[test]
    fn report_names_the_signal() {
        assert_eq!(
            reported(&Status::Signaled(9)),
            "mini_shell: command terminated by signal 9\n"
        );
    }

    #[test]
    fn report_names_a_missing_program() {
        // mini_shell's own words, not io::Error's: every port writes these
        // bytes itself, which is what keeps them byte-identical.
        assert_eq!(
            reported_for(&Status::NotFound, b"nosuchcmd"),
            "mini_shell: nosuchcmd: command not found\n"
        );
    }

    #[test]
    fn report_names_a_program_it_may_not_execute() {
        assert_eq!(
            reported_for(&Status::NotExecutable, b"/etc/passwd"),
            "mini_shell: /etc/passwd: permission denied\n"
        );
    }

    #[test]
    fn report_names_the_error_behind_an_unrunnable_command() {
        // The C port pairs this line with strerror(errno); the error is carried
        // in the Status here, so it is the one that failed rather than whatever
        // errno happens to hold.
        let cause = io::Error::from_raw_os_error(EAGAIN);
        let expected = format!("mini_shell: failed to run command: {cause}\n");
        assert_eq!(reported(&Status::Unrunnable(cause)), expected);
    }

    // --- is_exit_command / is_blank / trim ---

    #[test]
    fn exit_matches_the_bare_word() {
        assert!(is_exit_command(b"exit"));
    }

    #[test]
    fn exit_ignores_surrounding_whitespace() {
        assert!(is_exit_command(b"  exit"));
        assert!(is_exit_command(b"exit\t"));
        assert!(is_exit_command(b" \texit \t"));
    }

    #[test]
    fn exit_does_not_match_other_spellings() {
        // Each of these is a command like any other and goes to the
        // interpreter; matching them here would quietly diverge from what a real
        // shell does.
        assert!(!is_exit_command(b"EXIT"));
        assert!(!is_exit_command(b"exitx"));
        assert!(!is_exit_command(b"exit 3"));
        assert!(!is_exit_command(b"exi"));
        assert!(!is_exit_command(b""));
    }

    #[test]
    fn blank_is_empty_or_whitespace_only() {
        assert!(is_blank(b""));
        assert!(is_blank(b"   "));
        assert!(is_blank(b"\t \t"));
        assert!(!is_blank(b"ls"));
        assert!(!is_blank(b"  ls  "));
    }

    #[test]
    fn treats_a_vertical_tab_as_whitespace_like_the_other_ports() {
        // u8::is_ascii_whitespace omits \v, which C's isspace and the C++ port's
        // explicit set both include. Using it would make this line a command
        // here and the end of the loop there.
        assert!(is_exit_command(b"\x0bexit\x0c"));
        assert!(is_blank(b"\x0b\x0c"));
    }

    #[test]
    fn trim_keeps_interior_bytes_including_nuls() {
        assert_eq!(trim(b"  echo a\0b  "), b"echo a\0b");
    }

    // --- split ---

    #[test]
    fn one_word_is_the_program_alone() {
        assert_eq!(split(b"ls"), [b"ls"]);
    }

    #[test]
    fn later_words_are_arguments() {
        assert_eq!(
            split(b"echo hello world"),
            [&b"echo"[..], b"hello", b"world"]
        );
    }

    #[test]
    fn runs_of_whitespace_separate_exactly_once() {
        // Two arguments, not five: the empty slices between the separators are
        // not words. A shell that passed them along would hand echo blank
        // arguments.
        assert_eq!(split(b"echo   a  \t b"), [&b"echo"[..], b"a", b"b"]);
    }

    #[test]
    fn surrounding_whitespace_is_dropped_by_splitting() {
        assert_eq!(split(b"  \t ls -l \t "), [&b"ls"[..], b"-l"]);
    }

    #[test]
    fn a_vertical_tab_and_form_feed_separate_too() {
        // The same set trim uses, and the reason it is spelled out:
        // u8::is_ascii_whitespace omits \v.
        assert_eq!(split(b"echo\x0ba\x0cb"), [&b"echo"[..], b"a", b"b"]);
    }

    #[test]
    fn nothing_to_split_yields_no_words() {
        assert!(split(b"").is_empty());
        assert!(split(b"   \t ").is_empty());
    }

    #[test]
    fn shell_metacharacters_are_ordinary_words() {
        // The whole of the grammar: whitespace separates, everything else is a
        // byte in a word. There is no interpreter left to give these meaning.
        assert_eq!(split(b"echo a | wc"), [&b"echo"[..], b"a", b"|", b"wc"]);
        assert_eq!(
            split(b"echo * $HOME > out"),
            [&b"echo"[..], b"*", b"$HOME", b">", b"out"]
        );
        assert_eq!(split(b"echo \"a b\""), [&b"echo"[..], b"\"a", b"b\""]);
    }

    #[test]
    fn splitting_keeps_non_ascii_bytes() {
        assert_eq!(split(b"echo \xc3\xa9"), [&b"echo"[..], b"\xc3\xa9"]);
    }

    // --- run ---

    #[test]
    fn passes_commands_to_the_runner_in_order() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"echo one\necho two\nexit\n", &mut fake);

        assert!(run.result.is_ok());
        assert_eq!(
            fake.commands,
            [argv(&["echo", "one"]), argv(&["echo", "two"])]
        );
    }

    #[test]
    fn prompts_once_per_command_and_once_more_for_exit() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"ls\nexit\n", &mut fake);

        assert_eq!(run.out, "$ $ ");
        assert_eq!(run.err, "");
    }

    #[test]
    fn exit_stops_the_loop_and_leaves_later_lines_unread() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"exit\nrm -rf /\n", &mut fake);

        assert!(run.result.is_ok());
        assert!(fake.commands.is_empty());
    }

    #[test]
    fn end_of_input_stops_the_loop_and_ends_the_line() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"ls\n", &mut fake);

        assert!(run.result.is_ok());
        assert_eq!(fake.commands.len(), 1);
        // The prompt for the command, then the prompt that end of input
        // answered, then a newline so the cursor does not stop on the prompt's
        // line.
        assert_eq!(run.out, "$ $ \n");
    }

    #[test]
    fn runs_a_final_line_with_no_newline() {
        let mut fake = FakeRunner::default();
        run_shell(b"echo hi", &mut fake);

        assert_eq!(fake.commands, [argv(&["echo", "hi"])]);
    }

    #[test]
    fn exit_needs_no_trailing_newline() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"exit", &mut fake);

        assert!(run.result.is_ok());
        assert!(fake.commands.is_empty());
        // The loop ended at "exit", not at end of input, so no closing newline.
        assert_eq!(run.out, "$ ");
    }

    #[test]
    fn blank_lines_are_prompted_for_but_not_run() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"\n   \nls\nexit\n", &mut fake);

        assert_eq!(fake.commands, [argv(&["ls"])]);
        assert_eq!(run.out, "$ $ $ $ ");
    }

    #[test]
    fn strips_crlf_and_splits_on_an_interior_carriage_return() {
        let mut fake = FakeRunner::default();
        run_shell(b"echo a\rb\r\n", &mut fake);

        // The trailing "\r\n" is a line terminator and is stripped. The
        // interior '\r' is ASCII whitespace like any other, so it separates two
        // arguments — where the interpreter used to receive it inside the
        // command line.
        assert_eq!(fake.commands, [argv(&["echo", "a", "b"])]);
    }

    #[test]
    fn hands_the_runner_a_split_line_rather_than_a_command_line() {
        // Surrounding and interior whitespace is gone by the time the runner
        // sees it: splitting is mini_shell's job now, not an interpreter's.
        let mut fake = FakeRunner::default();
        run_shell(b"  ls   -l  /tmp \nexit\n", &mut fake);

        assert_eq!(fake.commands, [argv(&["ls", "-l", "/tmp"])]);
    }

    #[test]
    fn treats_shell_metacharacters_as_ordinary_arguments() {
        // No pipe: echo is run once, with three arguments.
        let mut fake = FakeRunner::default();
        run_shell(b"echo a | wc\nexit\n", &mut fake);

        assert_eq!(fake.commands, [argv(&["echo", "a", "|", "wc"])]);
    }

    #[test]
    fn reports_a_missing_program_by_name() {
        // The runner fails the way ExecRunner does when the exec failed.
        let mut fake = FakeRunner {
            statuses: vec![Err(io::Error::from(ErrorKind::NotFound))],
            ..FakeRunner::default()
        };
        let run = run_shell(b"nosuchcmd arg\nexit\n", &mut fake);

        assert!(run.result.is_ok());
        assert_eq!(run.err, "mini_shell: nosuchcmd: command not found\n");
    }

    #[test]
    fn reports_a_failed_command_and_keeps_going() {
        let mut fake = FakeRunner {
            statuses: vec![Ok(exited(3)), Ok(exited(0))],
            ..FakeRunner::default()
        };
        let run = run_shell(b"false\ntrue\nexit\n", &mut fake);

        assert!(run.result.is_ok());
        assert_eq!(fake.commands.len(), 2);
        assert_eq!(run.err, "mini_shell: command exited with status 3\n");
    }

    #[test]
    fn reports_a_signaled_command() {
        let mut fake = FakeRunner {
            statuses: vec![Ok(signaled(9))],
            ..FakeRunner::default()
        };
        let run = run_shell(b"sleep 100\nexit\n", &mut fake);

        assert_eq!(run.err, "mini_shell: command terminated by signal 9\n");
    }

    #[test]
    fn refuses_a_line_containing_a_nul_byte() {
        let mut fake = FakeRunner::default();
        // execvp takes NUL-terminated strings, so running this would run
        // "echo a" and silently drop the rest.
        let run = run_shell(b"echo a\0rm -rf /\nexit\n", &mut fake);

        assert!(run.result.is_ok());
        assert!(fake.commands.is_empty());
        assert_eq!(run.err, "mini_shell: command contains a NUL byte\n");
    }

    #[test]
    fn passes_non_ascii_bytes_through() {
        let mut fake = FakeRunner::default();
        run_shell(b"echo \xc3\xa9\nexit\n", &mut fake);

        assert_eq!(fake.commands, [argv(&["echo", "\u{e9}"])]);
    }

    #[test]
    fn empty_input_still_prompts_once() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"", &mut fake);

        assert!(run.result.is_ok());
        assert!(fake.commands.is_empty());
        assert_eq!(run.out, "$ \n");
    }

    #[test]
    fn writes_the_banner_before_the_first_prompt_when_asked() {
        let mut fake = FakeRunner::default();
        let run = drive(&mut &b"exit\n"[..], &mut fake, true);

        let mut banner = Vec::new();
        write_banner(&mut banner).expect("writing to a Vec cannot fail");
        assert!(!banner.is_empty());
        assert_eq!(run.out, text(banner) + "$ ");
    }

    #[test]
    fn retries_a_read_interrupted_by_a_signal() {
        let mut fake = FakeRunner::default();
        let mut input = InterruptedOnceReader {
            rest: b"echo hi\nexit\n",
            interrupted: false,
        };
        let run = drive(&mut input, &mut fake, false);

        assert!(run.result.is_ok());
        assert_eq!(fake.commands, [argv(&["echo", "hi"])]);
    }

    #[test]
    fn reports_a_read_error_on_the_input_stream() {
        let mut fake = FakeRunner::default();
        let run = drive(&mut FailingReader, &mut fake, false);

        assert!(matches!(run.result, Err(ShellError::Read(_))));
        assert_eq!(run.err, "");
        // Unlike the C++ port, which cannot tell a failed read from a clean end
        // of input and writes its closing newline first.
        assert_eq!(run.out, "$ ");
    }

    #[test]
    fn reports_a_write_error_on_the_prompt() {
        let mut fake = FakeRunner::default();
        let mut err = Vec::new();
        let mut opts = Options {
            show_banner: false,
            runner: &mut fake,
        };
        let result = run(&mut &b"exit\n"[..], &mut FailingWriter, &mut err, &mut opts);

        assert!(matches!(result, Err(ShellError::Write(_))));
        assert_eq!(text(err), "");
    }

    // --- ShellError ---

    #[test]
    fn every_error_has_a_label() {
        let failed = || io::Error::from_raw_os_error(EAGAIN);
        assert_eq!(
            ShellError::Read(failed()).label(),
            "error reading command input"
        );
        assert_eq!(ShellError::Write(failed()).label(), "error writing output");
    }

    #[test]
    fn only_a_stream_error_names_its_cause() {
        let cause = io::Error::from_raw_os_error(EAGAIN);
        assert_eq!(
            ShellError::Read(cause).to_string(),
            format!(
                "error reading command input: {}",
                io::Error::from_raw_os_error(EAGAIN)
            )
        );
    }
}
