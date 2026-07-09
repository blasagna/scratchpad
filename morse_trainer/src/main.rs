//! CLI entry point: parses key bindings, sets up the terminal, and runs the
//! event loop. Characters are keyed with two distinct inputs (a dot key and a
//! dash key); a pause longer than the timeout commits the current character.

use std::io::{self, Stdout};
use std::process::ExitCode;
use std::time::{Duration, Instant};

use clap::Parser;
use crossterm::event::{
    self, DisableMouseCapture, EnableMouseCapture, Event, KeyCode, KeyEventKind, MouseButton,
    MouseEventKind,
};
use crossterm::execute;
use crossterm::terminal::{
    EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode,
};
use ratatui::Terminal;
use ratatui::backend::CrosstermBackend;

use morse_trainer::app::App;
use morse_trainer::morse::InputEvent;
use morse_trainer::ui;

/// How often the event loop wakes to check the input timeout.
const POLL_INTERVAL: Duration = Duration::from_millis(50);

#[derive(Parser)]
#[command(
    name = "morse_trainer",
    about = "Practice Morse code in the terminal with two keys for dot and dash.",
    long_about = "A TUI for practicing Morse code. Key a character with two distinct \
                  inputs — a dot key and a dash key — and a pause longer than the timeout \
                  ends the character. Offers free-text entry and a prompted mode with \
                  progressive lessons and English phrases."
)]
struct Cli {
    /// Key that enters a dot.
    #[arg(long, default_value_t = 'j')]
    dot: char,

    /// Key that enters a dash.
    #[arg(long, default_value_t = 'k')]
    dash: char,

    /// Key that quits the program.
    #[arg(long, default_value_t = 'q')]
    quit: char,

    /// End-of-character pause timeout, in milliseconds.
    #[arg(long, default_value_t = 300, value_parser = clap::value_parser!(u64).range(1..))]
    timeout: u64,

    /// Also accept mouse buttons (left = dot, right = dash).
    #[arg(long)]
    mouse: bool,
}

/// Resolved key bindings for the session.
struct Keys {
    dot: char,
    dash: char,
    quit: char,
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    if cli.dot == cli.dash {
        eprintln!(
            "dot and dash keys must be different (both are {:?})",
            cli.dot
        );
        return ExitCode::FAILURE;
    }

    let mut terminal = match setup_terminal(cli.mouse) {
        Ok(t) => t,
        Err(err) => {
            eprintln!("failed to initialize terminal: {err}");
            return ExitCode::FAILURE;
        }
    };

    let result = run(&mut terminal, &cli);

    // Always attempt to restore the terminal, even if the loop errored.
    let restored = restore_terminal(cli.mouse);

    if let Err(err) = result {
        eprintln!("error: {err}");
        return ExitCode::FAILURE;
    }
    if let Err(err) = restored {
        eprintln!("failed to restore terminal: {err}");
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}

type Tui = Terminal<CrosstermBackend<Stdout>>;

fn setup_terminal(mouse: bool) -> io::Result<Tui> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    if mouse {
        execute!(stdout, EnableMouseCapture)?;
    }
    Terminal::new(CrosstermBackend::new(stdout))
}

fn restore_terminal(mouse: bool) -> io::Result<()> {
    let mut stdout = io::stdout();
    if mouse {
        execute!(stdout, DisableMouseCapture)?;
    }
    execute!(stdout, LeaveAlternateScreen)?;
    disable_raw_mode()
}

fn run(terminal: &mut Tui, cli: &Cli) -> io::Result<()> {
    let keys = Keys {
        dot: cli.dot,
        dash: cli.dash,
        quit: cli.quit,
    };
    let timeout = Duration::from_millis(cli.timeout);
    let legend = format!(
        "dot: {}   dash: {}   quit: {}   timeout: {}ms   |   ↑/↓ ⏎ menu   esc back   tab hint",
        display_key(keys.dot),
        display_key(keys.dash),
        display_key(keys.quit),
        cli.timeout,
    );

    let mut app = App::new();
    let mut last_input = Instant::now();

    while !app.should_quit {
        terminal.draw(|frame| ui::render(frame, &app, &legend))?;

        if event::poll(POLL_INTERVAL)? {
            match event::read()? {
                Event::Key(key) if key.kind == KeyEventKind::Press => {
                    if handle_key(&mut app, key.code, &keys) {
                        last_input = Instant::now();
                    }
                }
                Event::Mouse(mouse) if cli.mouse && app.in_practice() => {
                    if handle_mouse(&mut app, mouse.kind) {
                        last_input = Instant::now();
                    }
                }
                _ => {}
            }
        }

        // Legacy pause-timeout segmentation: a quiet gap ends the character.
        if app.has_pending() && last_input.elapsed() >= timeout {
            app.commit_sequence();
        }
    }

    Ok(())
}

/// Handles a key press. Returns `true` when a dot/dash was keyed, so the caller
/// can reset the input-timeout clock.
fn handle_key(app: &mut App, code: KeyCode, keys: &Keys) -> bool {
    match code {
        KeyCode::Char(c) if c == keys.quit => {
            app.should_quit = true;
            false
        }
        KeyCode::Char(c) if app.in_practice() && c == keys.dot => {
            app.push(InputEvent::Dot);
            true
        }
        KeyCode::Char(c) if app.in_practice() && c == keys.dash => {
            app.push(InputEvent::Dash);
            true
        }
        KeyCode::Up => {
            app.menu_up();
            false
        }
        KeyCode::Down => {
            app.menu_down();
            false
        }
        KeyCode::Enter => {
            app.menu_select();
            false
        }
        KeyCode::Esc => {
            app.back();
            false
        }
        KeyCode::Tab => {
            if app.in_practice() {
                app.toggle_hint();
            }
            false
        }
        _ => false,
    }
}

/// Handles a mouse event in practice mode. Returns `true` when a dot/dash was keyed.
fn handle_mouse(app: &mut App, kind: MouseEventKind) -> bool {
    match kind {
        MouseEventKind::Down(MouseButton::Left) => {
            app.push(InputEvent::Dot);
            true
        }
        MouseEventKind::Down(MouseButton::Right) => {
            app.push(InputEvent::Dash);
            true
        }
        _ => false,
    }
}

/// Renders a key for the legend, spelling out space since it is invisible.
fn display_key(c: char) -> String {
    if c == ' ' {
        "space".to_string()
    } else {
        c.to_string()
    }
}
