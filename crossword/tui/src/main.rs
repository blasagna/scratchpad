//! CLI entry point: reads the NYT cookie, sets up the terminal, starts the
//! fetch thread and runs the event loop.

use std::any::Any;
use std::io::{self, Stdout};
use std::panic::{self, AssertUnwindSafe};
use std::process::ExitCode;
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use chrono::Local;
use clap::Parser;
use crossterm::event::{self, DisableFocusChange, EnableFocusChange, Event, KeyEventKind};
use crossterm::execute;
use crossterm::terminal::{
    EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode,
};
use ratatui::Terminal;
use ratatui::backend::CrosstermBackend;

use crossword_core::app::App;
use crossword_core::sources::{Fetcher, Request, Response, Size, SourceError, nyt};
use crossword_core::store::Store;
use crossword_tui::{input, ui};

/// How often the loop wakes to redraw the timer.
const TICK: Duration = Duration::from_millis(250);

/// The name of the thread that serves fetch requests.
const FETCH_THREAD: &str = "fetch";

#[derive(Parser)]
#[command(
    name = "crossword",
    version,
    about = "Solve crosswords in the terminal with vim-style keys.",
    long_about = "Solve crosswords in the terminal with vim-style keys. Puzzles come in \
                  three sizes, after the NYT's Mini, Midi and Crossword, from free sources: \
                  the Daily Princetonian, the Guardian Quick and Universal. NYT puzzles \
                  need your own subscription cookie in NYT_S or in \
                  ~/.config/crossword/nyt-s."
)]
struct Cli {
    /// Open the puzzle list for this size.
    size: Option<Size>,

    /// Keep no cache and save no progress.
    #[arg(long)]
    no_save: bool,
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let store = if cli.no_save {
        None
    } else {
        Store::from_system_dirs()
    };

    // Restore the terminal before a panic message prints, or it lands in the
    // alternate screen and the shell is left in raw mode. The fetch thread
    // is the exception: it turns a panic into a failed request and the UI
    // runs on, so the terminal must stay as it is.
    let default_hook = panic::take_hook();
    panic::set_hook(Box::new(move |info| {
        if thread::current().name() == Some(FETCH_THREAD) {
            return;
        }
        let _ = restore_terminal();
        default_hook(info);
    }));

    let mut terminal = match setup_terminal() {
        Ok(t) => t,
        Err(err) => {
            eprintln!("failed to initialize terminal: {err}");
            return ExitCode::FAILURE;
        }
    };

    let result = run(&mut terminal, &cli, store);
    let restored = restore_terminal();

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

fn setup_terminal() -> io::Result<Tui> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    // Focus reports pause the timer while the window is in the background.
    // Terminals that do not send them simply never pause it.
    execute!(stdout, EnterAlternateScreen, EnableFocusChange)?;
    Terminal::new(CrosstermBackend::new(stdout))
}

fn restore_terminal() -> io::Result<()> {
    execute!(io::stdout(), DisableFocusChange, LeaveAlternateScreen)?;
    disable_raw_mode()
}

/// Starts the thread that serves listing and download requests, so the UI
/// never blocks on the network. A request that panics gets a failed
/// response, so the screen that waits for it does not wait for ever, and
/// the thread serves the next one.
fn spawn_fetcher(
    fetcher: Fetcher,
) -> io::Result<(mpsc::Sender<Request>, mpsc::Receiver<Response>)> {
    let (request_tx, request_rx) = mpsc::channel::<Request>();
    let (response_tx, response_rx) = mpsc::channel::<Response>();
    thread::Builder::new()
        .name(FETCH_THREAD.into())
        .spawn(move || {
            for request in request_rx {
                let fallback = request.clone();
                let handled = panic::catch_unwind(AssertUnwindSafe(|| {
                    fetcher.handle(request, Local::now().date_naive())
                }));
                let response = handled.unwrap_or_else(|payload| {
                    let error = format!("the fetch thread failed: {}", panic_text(&*payload));
                    fallback.failed(SourceError::Parse(error))
                });
                if response_tx.send(response).is_err() {
                    break;
                }
            }
        })?;
    Ok((request_tx, response_rx))
}

/// The message a panic was raised with.
fn panic_text(payload: &(dyn Any + Send)) -> &str {
    payload
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
        .unwrap_or("unknown error")
}

fn run(terminal: &mut Tui, cli: &Cli, store: Option<Store>) -> io::Result<()> {
    let (requests, responses) =
        spawn_fetcher(Fetcher::new(nyt::configured_cookie(), store.clone()))?;
    let mut app = App::new(store);
    if let Some(size) = cli.size {
        app.open_size(size);
    }

    loop {
        for request in app.take_requests() {
            // The fetch thread lives as long as this loop.
            let _ = requests.send(request);
        }
        let screen = terminal
            .draw(|frame| ui::render(frame, &app, Instant::now()))?
            .area;
        app.set_help_max_scroll(ui::help_max_scroll(screen));

        if event::poll(TICK)? {
            // Drain everything queued, so fast typing redraws once.
            loop {
                match event::read()? {
                    Event::Key(event) if event.kind == KeyEventKind::Press => {
                        if let Some(key) = input::key(event) {
                            app.handle_key(key, Instant::now());
                        }
                    }
                    Event::FocusLost => app.set_focus(false, Instant::now()),
                    Event::FocusGained => app.set_focus(true, Instant::now()),
                    _ => {}
                }
                if app.should_quit || !event::poll(Duration::ZERO)? {
                    break;
                }
            }
        }
        while let Ok(response) = responses.try_recv() {
            app.on_response(response, Instant::now());
        }
        app.tick(Instant::now());
        if app.should_quit {
            app.quit(Instant::now());
            return Ok(());
        }
    }
}
