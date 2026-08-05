//! A prototype shell: print a prompt, read one command per line, hand it to the
//! system command interpreter, and report the status of anything that did not
//! exit 0.
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
use std::os::unix::process::{CommandExt, ExitStatusExt};
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
    "commands run through the system shell; type 'exit' to quit",
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
/// Three variants where the C port's `ShellResult` has five. `getline` returns
/// `-1` for end of input, a read error, and an allocation failure alike, which
/// is the only reason `shell_run` has to consult `ferror`/`feof` and the only
/// reason `SHELL_ERR_NOMEM` exists. [`Read::read`] separates them in the type:
/// `Ok(0)` is end of input and `Err(_)` is a read error. The third case never
/// arrives here at all, because allocation failure aborts the process — a
/// deliberate divergence, recorded in `README.md`.
#[derive(Debug)]
pub enum ShellError {
    /// A read error on the command input stream.
    Read(io::Error),
    /// A write error on the output or error stream.
    Write(io::Error),
    /// No command interpreter is available.
    NoShell,
}

impl ShellError {
    /// The C port's `shell_result_str` labels, minus the two that have no
    /// variant here.
    pub fn label(&self) -> &'static str {
        match self {
            ShellError::Read(_) => "error reading command input",
            ShellError::Write(_) => "error writing output",
            ShellError::NoShell => "no command interpreter available",
        }
    }
}

impl fmt::Display for ShellError {
    /// Composes the two shapes `main` prints in the C port, so its caller needs
    /// one arm rather than two.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ShellError::Read(err) | ShellError::Write(err) => write!(f, "{}: {err}", self.label()),
            ShellError::NoShell => f.write_str(self.label()),
        }
    }
}

impl std::error::Error for ShellError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            ShellError::Read(err) | ShellError::Write(err) => Some(err),
            ShellError::NoShell => None,
        }
    }
}

/// How one command ended.
///
/// The same three cases the C and C++ ports decode out of a raw wait status.
/// The error behind `Unrunnable` is carried rather than left in a global
/// `errno` for the reporting code to pick up later.
#[derive(Debug)]
pub enum Status {
    /// The command ran and exited with this code.
    Exited(i32),
    /// The command was killed by this signal.
    Signaled(i32),
    /// The command never ran: the interpreter could not be started.
    Unrunnable(io::Error),
}

/// The seam that keeps [`run`] from spawning anything itself.
///
/// The C port spells it as a function pointer plus a `void *` context and the
/// C++ port as a `std::function`; here it is a trait, so the recording fake the
/// tests use is a plain struct whose fields they read afterwards.
pub trait Runner {
    fn run(&mut self, command: &[u8]) -> io::Result<ExitStatus>;
}

/// The one impure `Runner`: `/bin/sh -c <command>`, which is what libc's
/// `system()` does internally.
pub struct SystemRunner;

impl Runner for SystemRunner {
    fn run(&mut self, command: &[u8]) -> io::Result<ExitStatus> {
        Command::new("/bin/sh")
            // glibc's system() execs "/bin/sh" with argv[0] of "sh", so `echo $0`
            // agrees across the ports. Hardcoding the path rather than searching
            // $PATH is also what system() does.
            .arg0("sh")
            .arg("-c")
            // Not a String: the contract passes non-ASCII bytes through
            // unchanged, and a command line is not required to be UTF-8.
            .arg(OsStr::from_bytes(command))
            // status(), not output(): the command inherits our stdin, stdout,
            // and stderr, which is what makes `cat` and `ls | wc -l` work.
            .status()
    }
}

/// Whether a command interpreter exists at all, asked once at startup.
///
/// The stand-in for `system(NULL)`, which has no counterpart in Rust's standard
/// library. glibc implements that call as `do_system("exit 0")`, so this runs
/// the same thing. Worth the one fork: without an interpreter, every command
/// would fail the same way, one line of errno noise at a time.
pub fn interpreter_available() -> bool {
    let mut runner = SystemRunner;
    matches!(runner.run(b"exit 0"), Ok(status) if status.success())
}

/// What [`run`] needs beyond its three streams.
pub struct Options<'a> {
    pub show_banner: bool,
    pub runner: &'a mut dyn Runner,
}

/// Reduces what the runner reported to the three outcomes worth naming.
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
        // The C port's -1: the interpreter could not be started, so the error is
        // about the shell rather than about the command, which never ran.
        Err(err) => return Status::Unrunnable(err),
    };
    if let Some(signal) = status.signal() {
        return Status::Signaled(signal);
    }
    // A stopped child names neither, and reports as a clean exit, matching what
    // the C port's `if (WIFEXITED(raw))` leaves `code` at.
    Status::Exited(status.code().unwrap_or(0))
}

/// Writes one line about a command that did not succeed. A clean exit is silent.
pub fn report_status<W: Write>(err: &mut W, status: &Status) -> io::Result<()> {
    match status {
        Status::Exited(0) => Ok(()),
        Status::Exited(code) => writeln!(err, "{PROG_NAME}: command exited with status {code}"),
        Status::Signaled(signal) => {
            writeln!(err, "{PROG_NAME}: command terminated by signal {signal}")
        }
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
            // The interpreter takes a NUL-terminated string, so running this
            // would run `echo a` and silently drop the rest of
            // `echo a\0rm -rf /`. Refused rather than truncated.
            writeln!(err, "{PROG_NAME}: command contains a NUL byte").map_err(ShellError::Write)?;
            continue;
        }

        // The untrimmed line, as in C: trimming is how `exit` is recognized, not
        // something done to a command on its way to the interpreter.
        let status = decode_status(opts.runner.run(&line));
        report_status(err, &status).map_err(ShellError::Write)?;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A wait status as the runner returns one. The encoding is not something
    /// the standard library promises, so these build the layout every platform
    /// this runs on uses, and `tests/real_system.rs` checks that assumption
    /// against a real `/bin/sh`.
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
        commands: Vec<Vec<u8>>,
        statuses: Vec<ExitStatus>,
        next: usize,
    }

    impl Runner for FakeRunner {
        fn run(&mut self, command: &[u8]) -> io::Result<ExitStatus> {
            self.commands.push(command.to_vec());
            let status = self.statuses.get(self.next).copied().unwrap_or(exited(0));
            self.next += 1;
            Ok(status)
        }
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
        let mut err = Vec::new();
        report_status(&mut err, status).expect("writing to a Vec cannot fail");
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
    fn command_not_found_is_an_ordinary_exit() {
        // 127 is what the interpreter exits with when it cannot find the
        // command. It is not special-cased: the interpreter has already said so
        // itself.
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
    fn a_runner_error_means_the_command_never_ran() {
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

    // --- run ---

    #[test]
    fn passes_commands_to_the_runner_in_order() {
        let mut fake = FakeRunner::default();
        let run = run_shell(b"echo one\necho two\nexit\n", &mut fake);

        assert!(run.result.is_ok());
        assert_eq!(fake.commands, [b"echo one".to_vec(), b"echo two".to_vec()]);
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

        assert_eq!(fake.commands, [b"echo hi".to_vec()]);
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

        assert_eq!(fake.commands, [b"ls".to_vec()]);
        assert_eq!(run.out, "$ $ $ $ ");
    }

    #[test]
    fn strips_crlf_but_keeps_an_interior_carriage_return() {
        let mut fake = FakeRunner::default();
        run_shell(b"echo a\rb\r\n", &mut fake);

        assert_eq!(fake.commands, [b"echo a\rb".to_vec()]);
    }

    #[test]
    fn hands_the_interpreter_the_untrimmed_line() {
        // Trimming is how `exit` is recognized, not something done to a command.
        let mut fake = FakeRunner::default();
        run_shell(b"  ls  \nexit\n", &mut fake);

        assert_eq!(fake.commands, [b"  ls  ".to_vec()]);
    }

    #[test]
    fn reports_a_failed_command_and_keeps_going() {
        let mut fake = FakeRunner {
            statuses: vec![exited(3), exited(0)],
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
            statuses: vec![signaled(9)],
            ..FakeRunner::default()
        };
        let run = run_shell(b"sleep 100\nexit\n", &mut fake);

        assert_eq!(run.err, "mini_shell: command terminated by signal 9\n");
    }

    #[test]
    fn refuses_a_line_containing_a_nul_byte() {
        let mut fake = FakeRunner::default();
        // The interpreter takes a NUL-terminated string, so running this would
        // run "echo a" and silently drop the rest.
        let run = run_shell(b"echo a\0rm -rf /\nexit\n", &mut fake);

        assert!(run.result.is_ok());
        assert!(fake.commands.is_empty());
        assert_eq!(run.err, "mini_shell: command contains a NUL byte\n");
    }

    #[test]
    fn passes_non_ascii_bytes_through() {
        let mut fake = FakeRunner::default();
        run_shell(b"echo \xc3\xa9\nexit\n", &mut fake);

        assert_eq!(fake.commands, [b"echo \xc3\xa9".to_vec()]);
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
        assert_eq!(fake.commands, [b"echo hi".to_vec()]);
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
        assert_eq!(
            ShellError::NoShell.label(),
            "no command interpreter available"
        );
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
        // No trailing colon: there is no errno behind a missing interpreter.
        assert_eq!(
            ShellError::NoShell.to_string(),
            "no command interpreter available"
        );
    }
}
