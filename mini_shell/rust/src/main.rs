//! CLI for the prototype shell: reads commands from stdin and runs each through
//! the system command interpreter.

use std::fs::File;
use std::io::{self, BufWriter, Write};
use std::os::fd::AsFd;
use std::process::ExitCode;

use clap::Parser;

use mini_shell::{Options, PROG_NAME, ShellError, SystemRunner, interpreter_available, run};

#[derive(Parser)]
#[command(
    name = "mini_shell",
    about = "A prototype shell that runs each line through the system command interpreter.",
    long_about = "A prototype shell. Prints a '$' prompt, reads one command per line,\n\
                  hands it to the system command interpreter, and reports the exit status\n\
                  of any command that does not succeed. Repeats until you type 'exit' or\n\
                  close the input.\n\
                  \n\
                  Every command runs in a fresh subshell, so state a command sets --\n\
                  the working directory, an environment variable -- is gone by the next\n\
                  prompt. 'cd' therefore appears to do nothing.",
    after_help = "Commands come from stdin, so the prompt and banner are printed whether\n\
                  or not that is a terminal."
)]
struct Cli {
    /// Skip the startup banner.
    #[arg(long)]
    no_banner: bool,
}

fn main() -> ExitCode {
    // clap owns every usage error here and exits 2 for one, so this port has no
    // usage-error path of its own. Commands come from stdin, never from argv, so
    // there is no positional to declare and a stray operand is clap's to reject.
    let cli = Cli::parse();

    // The stand-in for the C and C++ ports' system(NULL). Asking once is worth
    // it: without an interpreter, every command would fail the same way, one
    // line of errno noise at a time.
    if !interpreter_available() {
        eprintln!("{PROG_NAME}: {}", ShellError::NoShell);
        return ExitCode::FAILURE;
    }

    // Read the command input unbuffered, so a command inherits the input
    // mini_shell has not consumed yet: `printf 'cat\necho done\n' | mini_shell`
    // must hand `cat` the `echo done` line. io::stdin() is always a BufReader
    // and would pull the whole pipe in before the first fork, and there is no
    // way to turn that off -- the C ports' setvbuf has no counterpart -- so take
    // a duplicate of fd 0 as a plain File instead. dup(2) shares the file
    // description and therefore the offset, so the child still sees exactly what
    // is left. No unsafe needed: File::from_raw_fd(0) would be, and would close
    // fd 0 when the File dropped.
    let stdin = match io::stdin().as_fd().try_clone_to_owned() {
        Ok(fd) => fd,
        Err(err) => {
            eprintln!("{PROG_NAME}: cannot duplicate stdin: {err}");
            return ExitCode::FAILURE;
        }
    };
    let mut input = File::from(stdin);

    // An explicit handle rather than print!, so a closed pipe is an ordinary
    // error and exit 1 instead of a panic and exit 101. Correctness does not
    // rest on the buffering: the loop flushes before every read.
    let mut out = BufWriter::new(io::stdout().lock());
    let mut err = io::stderr().lock();

    let mut runner = SystemRunner;
    let mut opts = Options {
        show_banner: !cli.no_banner,
        runner: &mut runner,
    };

    // The trailing flush is explicit because a dropped BufWriter swallows the
    // error it would report.
    let result = run(&mut input, &mut out, &mut err, &mut opts)
        .and_then(|()| out.flush().map_err(ShellError::Write));

    match result {
        Ok(()) => ExitCode::SUCCESS,
        // Display composes the label and the underlying error, so both of the C
        // port's shapes come out of one arm.
        Err(failure) => {
            eprintln!("{PROG_NAME}: {failure}");
            ExitCode::FAILURE
        }
    }
}
