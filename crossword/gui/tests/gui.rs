//! Drives the GUI headlessly with `iced_test`: it finds text, clicks widgets
//! and checks what the shared core does with the messages they produce.
//!
//! With `CROSSWORD_GUI_SNAPSHOTS=<dir>` set, each test also saves a PNG of
//! what it rendered, for a look at the GUI without a window:
//!
//! ```sh
//! CROSSWORD_GUI_SNAPSHOTS=/tmp/shots ICED_TEST_BACKEND=tiny-skia \
//!     cargo test -p crossword_gui --test gui
//! ```

use std::path::PathBuf;

use crossword_core::app::{App, Mode, Screen};
use crossword_core::keys::typed;
use crossword_core::puzzle::{ClueData, Direction, Meta, PuzzleData};
use crossword_core::sources::{Fetcher, PuzzleRef, Response, Size, SourceError, SourceId};
use crossword_gui::{Gui, Message};
use iced::{Size as Window, Theme};
use iced_test::simulator::Simulator;

const WINDOW: Window = Window::new(1200.0, 820.0);

/// Renders the GUI, lets `act` interact with it, and returns the messages
/// the interaction produced. Saves a snapshot when asked to.
fn interact(gui: &Gui, name: &str, act: impl FnOnce(&mut Simulator<'_, Message>)) -> Vec<Message> {
    let mut ui = Simulator::with_size(iced::Settings::default(), WINDOW, gui.view());
    if let Ok(dir) = std::env::var("CROSSWORD_GUI_SNAPSHOTS") {
        // `matches_image` writes only when no image exists yet, and compares
        // otherwise. These snapshots are for looking at, so replace any old
        // one (named `<name>-<renderer>.png`) first.
        let dir = PathBuf::from(dir);
        for old in std::fs::read_dir(&dir).into_iter().flatten().flatten() {
            if old
                .file_name()
                .to_string_lossy()
                .starts_with(&format!("{name}-"))
            {
                let _ = std::fs::remove_file(old.path());
            }
        }
        let snapshot = ui.snapshot(&Theme::Light).expect("snapshot");
        assert!(snapshot.matches_image(dir.join(name)).expect("write"));
    }
    act(&mut ui);
    ui.into_messages().collect()
}

fn apply(gui: &mut Gui, messages: Vec<Message>) {
    for message in messages {
        let _ = gui.update(message);
    }
}

fn reference() -> PuzzleRef {
    PuzzleRef {
        source: SourceId::Universal,
        id: "2026-10-08".into(),
        date: chrono::NaiveDate::from_ymd_opt(2026, 10, 8),
        title: Some("Tiny Test".into()),
    }
}

/// `CAT / ARE / #EE`, with a circle in the bottom-right square.
fn small_data() -> PuzzleData {
    let clue = |direction, number, text: &str| ClueData {
        direction,
        number,
        text: text.into(),
    };
    PuzzleData {
        meta: Meta {
            title: "Tiny Test".into(),
            author: "Ada Lovelace".into(),
            editor: String::new(),
            copyright: String::new(),
            date: Some("2026-10-08".into()),
        },
        width: 3,
        height: 3,
        grid: "CATARE#EE"
            .chars()
            .map(|c| (c != '#').then(|| c.to_string()))
            .collect(),
        circled: vec![8],
        alternates: Default::default(),
        clues: vec![
            clue(Direction::Across, 1, "Feline"),
            clue(Direction::Across, 4, "Exist"),
            clue(Direction::Across, 5, "Letter pair"),
            clue(Direction::Down, 1, "Golden State, briefly"),
            clue(Direction::Down, 2, "Painting, e.g."),
            clue(Direction::Down, 3, "Golf peg"),
        ],
    }
}

/// A GUI on the solve screen of the small puzzle, reached the way a player
/// reaches it: a size, a listing, a click on an item, a download.
fn solving() -> Gui {
    let mut gui = Gui::new(App::new(None), None);
    apply(
        &mut gui,
        vec![
            Message::Size(Size::Crossword),
            Message::Fetched(Box::new(Response::Listed(
                SourceId::Universal,
                Ok(vec![reference()]),
            ))),
            Message::Item(0),
            Message::Fetched(Box::new(Response::Fetched(reference(), Ok(small_data())))),
        ],
    );
    assert_eq!(gui.app().screen, Screen::Solve);
    gui
}

fn keys(gui: &mut Gui, text: &str) {
    apply(gui, typed(text).into_iter().map(Message::Key).collect());
}

#[test]
fn home_lists_sizes_and_a_click_opens_one() {
    let mut gui = Gui::new(App::new(None), None);
    let messages = interact(&gui, "home", |ui| {
        assert!(ui.find("Guardian Quick · NYT Midi").is_ok());
        ui.click("Midi").expect("Midi button");
    });
    apply(&mut gui, messages);
    assert_eq!(gui.app().screen, Screen::Browse);
    assert_eq!(gui.app().browse.as_ref().unwrap().size, Size::Midi);
}

#[test]
fn browse_shows_tabs_items_and_failures() {
    let mut gui = Gui::new(App::new(None), None);
    apply(
        &mut gui,
        vec![
            Message::Size(Size::Crossword),
            Message::Fetched(Box::new(Response::Listed(
                SourceId::Universal,
                Ok(vec![reference()]),
            ))),
            Message::Fetched(Box::new(Response::Listed(
                SourceId::NytDaily,
                Err(SourceError::NeedsCookie),
            ))),
        ],
    );
    let messages = interact(&gui, "browse", |ui| {
        assert!(ui.find("Thu Oct 8, 2026 · Tiny Test").is_ok());
        ui.click("NYT Crossword").expect("NYT tab");
    });
    apply(&mut gui, messages);
    let messages = interact(&gui, "browse-nyt", |ui| {
        ui.click("Try again").expect("retry button");
    });
    assert!(matches!(messages[..], [Message::Reload]));
}

#[test]
fn clicking_an_item_requests_its_puzzle() {
    let mut gui = Gui::new(App::new(None), None);
    apply(
        &mut gui,
        vec![
            Message::Size(Size::Crossword),
            Message::Fetched(Box::new(Response::Listed(
                SourceId::Universal,
                Ok(vec![reference()]),
            ))),
        ],
    );
    let messages = interact(&gui, "browse-click", |ui| {
        ui.click("Thu Oct 8, 2026 · Tiny Test").expect("item");
    });
    apply(&mut gui, messages);
    assert_eq!(gui.app().loading, Some(reference()));
}

#[test]
fn solve_screen_shows_clues_and_a_clue_click_moves_the_cursor() {
    let mut gui = solving();
    let messages = interact(&gui, "solve", |ui| {
        assert!(ui.find("Feline").is_ok());
        assert!(ui.find("NORMAL").is_ok());
        ui.click("Golf peg").expect("3-Down clue");
    });
    apply(&mut gui, messages);
    let game = &gui.app().solve.as_ref().unwrap().game;
    assert_eq!((game.cursor(), game.direction()), (2, Direction::Down));
}

#[test]
fn keys_drive_the_shared_vim_model() {
    let mut gui = solving();
    keys(&mut gui, "icat");
    let solve = gui.app().solve.as_ref().unwrap();
    assert_eq!(solve.mode, Mode::Insert);
    assert_eq!(solve.game.letter(2), "T");
    interact(&gui, "insert", |ui| {
        assert!(ui.find("INSERT").is_ok());
        assert!(ui.find("3/8").is_ok());
    });
    keys(&mut gui, "⎋:check puzzle");
    interact(&gui, "command", |ui| {
        assert!(ui.find(":check puzzle▏").is_ok());
    });
    keys(&mut gui, "⏎");
    assert!(gui.app().message.as_ref().unwrap().text.contains("right"));
}

#[test]
fn grid_clicks_move_the_cursor_and_switch_direction() {
    let mut gui = solving();
    apply(&mut gui, vec![Message::Cell(4)]);
    let game = &gui.app().solve.as_ref().unwrap().game;
    assert_eq!((game.cursor(), game.direction()), (4, Direction::Across));
    apply(&mut gui, vec![Message::Cell(4)]);
    let game = &gui.app().solve.as_ref().unwrap().game;
    assert_eq!(game.direction(), Direction::Down);
}

#[test]
fn help_opens_from_its_button_and_a_click_closes_it() {
    let mut gui = solving();
    let messages = interact(&gui, "solve-help-button", |ui| {
        ui.click("? Help").expect("help button");
    });
    apply(&mut gui, messages);
    assert!(gui.app().show_help);
    let messages = interact(&gui, "help", |ui| {
        assert!(ui.find("Normal mode").is_ok());
        ui.click("Esc or a click closes").expect("help card");
    });
    apply(&mut gui, messages);
    assert!(!gui.app().show_help);
}

#[test]
fn back_buttons_walk_up_the_screens() {
    let mut gui = solving();
    let messages = interact(&gui, "solve-back", |ui| {
        ui.click("← List").expect("back to list");
    });
    apply(&mut gui, messages);
    assert_eq!(gui.app().screen, Screen::Browse);
    let messages = interact(&gui, "browse-back", |ui| {
        ui.click("← Sizes").expect("back to sizes");
    });
    apply(&mut gui, messages);
    assert_eq!(gui.app().screen, Screen::Home);
}

/// Downloads today's puzzle from two free sources, a 15x15 and a British
/// 13x13, and renders each. Needs the network, so it is ignored by default:
/// `cargo test -p crossword_gui --test gui -- --ignored`.
#[test]
#[ignore = "needs the network"]
fn live_puzzles_render() {
    let fetcher = Fetcher::new(None, None);
    let today = chrono::Local::now().date_naive();
    for (size, source) in [
        (Size::Crossword, SourceId::Universal),
        (Size::Midi, SourceId::GuardianQuick),
    ] {
        let listing = fetcher.list(source, today).expect("listing");
        let (puzzle, data) = listing
            .iter()
            .take(3)
            .find_map(|r| fetcher.fetch(r).ok().map(|d| (r.clone(), d)))
            .expect("a recent puzzle");
        let first_clue = crossword_core::puzzle::Puzzle::new(data.clone())
            .unwrap()
            .entries()[0]
            .clue
            .clone();
        let mut gui = Gui::new(App::new(None), None);
        apply(
            &mut gui,
            vec![
                Message::Size(size),
                Message::Fetched(Box::new(Response::Listed(source, Ok(listing.clone())))),
                Message::Item(listing.iter().position(|r| *r == puzzle).unwrap()),
                Message::Fetched(Box::new(Response::Fetched(puzzle, Ok(data)))),
            ],
        );
        keys(&mut gui, "iabc");
        interact(&gui, source.slug(), |ui| {
            assert!(ui.find(first_clue.as_str()).is_ok(), "{first_clue:?}");
        });
    }
}
