//! GUI entry point: parses the command line and runs the iced application.

use clap::Parser;

use crossword_core::app::App;
use crossword_core::sources::{Fetcher, Size, nyt};
use crossword_core::store::Store;
use crossword_gui::Gui;

#[derive(Parser)]
#[command(
    name = "crossword-gui",
    version,
    about = "Solve crosswords in a window with vim-style keys.",
    long_about = "Solve crosswords in a window with vim-style keys or the mouse. The GUI \
                  shares every rule, source and saved puzzle with the terminal app. NYT \
                  puzzles need your own subscription cookie in NYT_S or in \
                  ~/.config/crossword/nyt-s."
)]
struct Cli {
    /// Open the puzzle list for this size.
    size: Option<Size>,

    /// Keep no cache and save no progress.
    #[arg(long)]
    no_save: bool,
}

fn main() -> iced::Result {
    let cli = Cli::parse();
    let store = if cli.no_save {
        None
    } else {
        Store::from_system_dirs()
    };
    let size = cli.size;

    iced::application(
        move || {
            let mut app = App::new(store.clone());
            if let Some(size) = size {
                app.open_size(size);
            }
            let mut gui = Gui::new(
                app,
                Some(Fetcher::new(nyt::configured_cookie(), store.clone())),
            );
            let start = gui.flush();
            (gui, start)
        },
        Gui::update,
        Gui::view,
    )
    .title(Gui::title)
    .subscription(Gui::subscription)
    .theme(Gui::theme)
    // The close button saves progress before the window goes away.
    .exit_on_close_request(false)
    .window_size((1200.0, 820.0))
    .run()
}
