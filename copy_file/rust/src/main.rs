//! CLI for the file copier: copies a source file to a destination, like `cp`.

use std::process::ExitCode;

use clap::Parser;

use copy_file::{CopyMethod, copy};

#[derive(Parser)]
#[command(
    name = "copy_file",
    about = "Copies the contents of a source file to a destination.",
    long_about = "Copies the contents of a source file to a destination, like cp.\n\n\
                  Relative paths and a leading '~' (expanded via $HOME) are \
                  supported. When the destination is an existing directory, the \
                  source is copied into it under its base file name."
)]
struct Cli {
    /// Copy with std::fs::copy instead of the streaming std::io::copy.
    #[arg(long)]
    fs: bool,

    /// Source file to read from.
    source: String,

    /// Destination file to create, or a directory to copy into.
    dest: String,
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let method = if cli.fs {
        CopyMethod::Fs
    } else {
        CopyMethod::Stream
    };

    match copy(&cli.source, &cli.dest, method) {
        Ok(dest) => {
            // Show the resolved destination so a directory target reveals the
            // real path.
            println!("copied '{}' to '{}'", cli.source, dest.display());
            ExitCode::SUCCESS
        }
        Err(err) => {
            eprintln!("copy_file: {err}");
            ExitCode::FAILURE
        }
    }
}
