//! CLI for the prototype shell: reads commands from stdin and runs each one as
//! a program with its arguments.

use std::fs::File;
use std::io::{self, BufWriter, Write};
use std::os::fd::AsFd;
use std::process::ExitCode;

use clap::Parser;

use mini_shell::{ExecRunner, Options, PROG_NAME, ShellError, run};

#[derive(Parser)]
#[command(
    name = "mini_shell",
    about = "A prototype shell that runs one program per line.",
    long_about = "A prototype shell. Prints a '$' prompt, reads one command per line,\n\
                  runs it, and reports the exit status of any command that does not\n\
                  succeed. Repeats until you type 'exit' or close the input.\n\
                  \n\
                  A line is split on whitespace. The first word is a program, looked up\n\
                  on PATH and run directly; the rest are its arguments, passed through\n\
                  exactly as typed. There is no shell in between, so there are no pipes,\n\
                  no redirection, no globbing, no quoting, and no variable expansion --\n\
                  'echo a | wc' prints 'a | wc'. Each command is a fresh process, so\n\
                  state it sets is gone by the next prompt, and 'cd' is not found at all.",
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

    let mut runner = ExecRunner;
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
