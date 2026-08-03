//! The `matrix_ops` command line.
//!
//! Unlike the C and C++ ports, which hand-write their option parsing so their
//! diagnostics agree byte for byte, this port uses clap in the ordinary way and
//! accepts clap's wording. The shape rules and the output format still match.
//! See `README.md` for the full list of divergences.

use std::fmt::Write as _;
use std::io::{self, BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, ValueEnum};
use matrix_ops::{DEFAULT_PRECISION, MAX_PRECISION, Matrix, parse, render};

/// Exit status for a usage error, as in the C and C++ ports. clap already uses
/// this for the errors it reports itself.
const EXIT_USAGE: u8 = 2;

#[derive(Parser)]
#[command(
    name = "matrix_ops",
    about = "Performs an operation on 2D matrices of real numbers.",
    long_about = "Performs an operation on 2D matrices of real numbers and prints \
                  the result.\n\n\
                  Each --values or --file introduces one operand. Dimensions are \
                  optional: without them the layout decides, one line of values \
                  being a row vector and several lines being rows. Given both \
                  --rows and --cols the values are reshaped row-major and their \
                  count must be exactly rows x cols; given one, the other is \
                  derived. Rows of differing length are always an error.",
    after_help = "The Nth --rows and --cols describe the Nth operand, and inline \
                  operands are ordered before file ones.\n\n\
                  Examples:\n  \
                  matrix_ops add --values \"1 2 3\" --values \"4 5 6\"\n  \
                  matrix_ops mul --values \"1 2 3 4 5 6\" --file b.txt \\\n                 \
                  --rows 2 --rows 3 --cols 3 --cols 2\n  \
                  matrix_ops scale --scalar 2.5 --file a.txt"
)]
struct Cli {
    /// The operation to perform
    operation: Operation,

    /// Values separated by whitespace or newlines
    #[arg(short, long, allow_hyphen_values = true)]
    values: Vec<String>,

    /// Read the values from a file ('-' for stdin)
    #[arg(short, long)]
    file: Vec<PathBuf>,

    /// Rows for the corresponding operand
    #[arg(short, long, value_parser = clap::value_parser!(u64).range(1..))]
    rows: Vec<u64>,

    /// Columns for the corresponding operand
    #[arg(short, long, value_parser = clap::value_parser!(u64).range(1..))]
    cols: Vec<u64>,

    /// The multiplier for 'scale'
    #[arg(short = 'k', long, allow_hyphen_values = true)]
    scalar: Option<f64>,

    /// Decimal places in the output, trailing zeros trimmed
    #[arg(
        short,
        long,
        default_value_t = DEFAULT_PRECISION as u64,
        value_parser = clap::value_parser!(u64).range(0..=MAX_PRECISION as u64),
    )]
    precision: u64,
}

#[derive(Copy, Clone, PartialEq, Eq, ValueEnum)]
enum Operation {
    /// Element-wise sum of two matrices of the same shape
    Add,
    /// Element-wise difference of two matrices of the same shape
    Sub,
    /// Matrix product; the first operand's column count must equal the
    /// second operand's row count
    Mul,
    /// Multiplies one matrix by --scalar
    Scale,
}

impl Operation {
    fn name(self) -> &'static str {
        match self {
            Operation::Add => "add",
            Operation::Sub => "sub",
            Operation::Mul => "mul",
            Operation::Scale => "scale",
        }
    }

    fn operand_count(self) -> usize {
        match self {
            Operation::Scale => 1,
            _ => 2,
        }
    }
}

/// A failure that has already been reduced to a message and an exit status.
///
/// The split is the same one the other ports make: a `Usage` failure always
/// traces back to what was typed, so a dimension mismatch belongs here rather
/// than with the operational failures.
enum Failure {
    Usage(String),
    Operational(String),
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    match run(&cli) {
        Ok(()) => ExitCode::SUCCESS,
        Err(Failure::Usage(message)) => {
            eprintln!("error: {message}");
            ExitCode::from(EXIT_USAGE)
        }
        Err(Failure::Operational(message)) => {
            eprintln!("matrix_ops: {message}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: &Cli) -> Result<(), Failure> {
    let operands = load_operands(cli)?;
    let result = apply(cli, &operands)?;

    // Written through an explicit handle rather than `print!`, so a closed pipe
    // is an ordinary error and exit 1 instead of a panic and exit 101.
    let stdout = io::stdout().lock();
    let mut out = BufWriter::new(stdout);
    let text = render(&result, cli.precision as usize);
    out.write_all(text.as_bytes())
        .and_then(|()| out.flush())
        .map_err(|e| Failure::Operational(format!("error writing output: {e}")))
}

/// Reads every operand, applying the dimensions that describe it.
///
/// The Nth `--rows`/`--cols` describes the Nth operand. Inline operands are
/// ordered before file ones, which is the one place this port's argument
/// handling is visibly looser than C's: there, dimensions and operands are
/// interleaved in the order typed.
fn load_operands(cli: &Cli) -> Result<Vec<Matrix>, Failure> {
    let expected = cli.operation.operand_count();
    let given = cli.values.len() + cli.file.len();
    if given != expected {
        return Err(Failure::Usage(format!(
            "'{}' takes {expected} {}, but {given} {} given",
            cli.operation.name(),
            plural(expected, "matrix", "matrices"),
            plural(given, "was", "were"),
        )));
    }
    if cli.rows.len() > expected || cli.cols.len() > expected {
        return Err(Failure::Usage(format!(
            "'{}' takes {expected} {}, so at most that many --rows and --cols \
             may be given",
            cli.operation.name(),
            plural(expected, "matrix", "matrices"),
        )));
    }

    match (cli.operation, cli.scalar) {
        (Operation::Scale, None) => {
            return Err(Failure::Usage("'scale' requires --scalar".to_string()));
        }
        (Operation::Scale, Some(k)) if !k.is_finite() => {
            return Err(Failure::Usage(
                "invalid value for --scalar (expected a finite number)".to_string(),
            ));
        }
        (op, Some(_)) if op != Operation::Scale => {
            return Err(Failure::Usage(
                "--scalar applies only to 'scale'".to_string(),
            ));
        }
        _ => {}
    }

    let sources = cli
        .values
        .iter()
        .map(|text| (Source::Inline(text), "--values".to_string()))
        .chain(cli.file.iter().map(|path| {
            let label = if path == Path::new("-") {
                "<stdin>".to_string()
            } else {
                path.display().to_string()
            };
            (Source::File(path), label)
        }));

    let mut operands = Vec::with_capacity(expected);
    for (i, (source, label)) in sources.enumerate() {
        let text = source.read(&label)?;
        let rows = cli.rows.get(i).map(|n| *n as usize);
        let cols = cli.cols.get(i).map(|n| *n as usize);
        let matrix =
            parse(&text, rows, cols).map_err(|e| Failure::Usage(format!("{label}: {e}")))?;
        operands.push(matrix);
    }
    Ok(operands)
}

enum Source<'a> {
    Inline(&'a str),
    File(&'a Path),
}

impl Source<'_> {
    fn read(&self, label: &str) -> Result<String, Failure> {
        match self {
            Source::Inline(text) => Ok((*text).to_string()),
            Source::File(path) if *path == Path::new("-") => {
                let mut text = String::new();
                io::stdin()
                    .read_to_string(&mut text)
                    .map(|_| text)
                    .map_err(|e| Failure::Operational(format!("{label}: {e}")))
            }
            Source::File(path) => std::fs::read_to_string(path)
                .map_err(|e| Failure::Operational(format!("{label}: {e}"))),
        }
    }
}

fn apply(cli: &Cli, operands: &[Matrix]) -> Result<Matrix, Failure> {
    let result = match cli.operation {
        Operation::Add => operands[0].add(&operands[1]),
        Operation::Sub => operands[0].sub(&operands[1]),
        Operation::Mul => operands[0].mul(&operands[1]),
        // Validated in load_operands, which rejects 'scale' without --scalar.
        Operation::Scale => Ok(operands[0].scale(cli.scalar.unwrap_or_default())),
    };

    result.map_err(|e| {
        let mut message = String::new();
        let _ = write!(
            message,
            "cannot {} these matrices: {e}",
            cli.operation.name()
        );
        Failure::Usage(message)
    })
}

fn plural(n: usize, one: &'static str, many: &'static str) -> &'static str {
    if n == 1 { one } else { many }
}
