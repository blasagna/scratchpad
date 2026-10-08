//! Renders each screen through a headless `TestBackend` to confirm the UI
//! draws without panicking and shows the expected content. This is the
//! automated stand-in for a manual TUI smoke test.

use std::time::Instant;

use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use crossword::app::{App, Screen};
use crossword::puzzle::{ClueData, Direction, Meta, PuzzleData};
use crossword::sources::{PuzzleRef, Response, Size, SourceError, SourceId};
use ratatui::Terminal;
use ratatui::backend::TestBackend;

fn render(app: &App, width: u16, height: u16) -> Vec<String> {
    let mut terminal = Terminal::new(TestBackend::new(width, height)).unwrap();
    terminal
        .draw(|frame| crossword::ui::render(frame, app, Instant::now()))
        .unwrap();
    let buffer = terminal.backend().buffer().clone();
    (0..height)
        .map(|y| (0..width).map(|x| buffer[(x, y)].symbol()).collect())
        .collect()
}

fn screen(app: &App, width: u16, height: u16) -> String {
    render(app, width, height).join("\n")
}

fn press(app: &mut App, keys: &str) {
    for c in keys.chars() {
        let code = match c {
            '⎋' => KeyCode::Esc,
            '⏎' => KeyCode::Enter,
            c => KeyCode::Char(c),
        };
        app.handle_key(KeyEvent::new(code, KeyModifiers::NONE), Instant::now());
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
        clues: vec![
            clue(Direction::Across, 1, "Feline"),
            clue(Direction::Across, 4, "Exist"),
            clue(Direction::Across, 5, "Letter pair"),
            clue(Direction::Down, 1, "Golden State, briefly"),
            clue(Direction::Down, 2, "Exist"),
            clue(Direction::Down, 3, "Golf peg"),
        ],
    }
}

/// A fully open 15×15, to exercise the layout choice at daily size.
fn big_data() -> PuzzleData {
    PuzzleData {
        meta: Meta::default(),
        width: 15,
        height: 15,
        grid: vec![Some("A".to_string()); 225],
        circled: Vec::new(),
        clues: Vec::new(),
    }
}

fn solving(data: PuzzleData) -> App {
    let mut app = App::new(None);
    app.open_size(Size::Crossword);
    app.on_response(
        Response::Listed(SourceId::Universal, Ok(vec![reference()])),
        Instant::now(),
    );
    press(&mut app, "⏎");
    app.on_response(Response::Fetched(reference(), Ok(data)), Instant::now());
    assert_eq!(app.screen, Screen::Solve);
    app
}

#[test]
fn home_lists_the_three_sizes_and_their_sources() {
    let app = App::new(None);
    let text = screen(&app, 80, 24);
    for word in [
        "crossword",
        "Mini",
        "Midi",
        "Crossword",
        "Guardian Quick",
        "Universal",
    ] {
        assert!(text.contains(word), "missing {word:?}:\n{text}");
    }
}

#[test]
fn browse_shows_tabs_items_and_failures() {
    let mut app = App::new(None);
    app.open_size(Size::Mini);
    assert!(screen(&app, 80, 24).contains("Loading…"));

    let item = PuzzleRef {
        source: SourceId::PrincetonianMini,
        id: "abc-1".into(),
        date: chrono::NaiveDate::from_ymd_opt(2026, 4, 3),
        title: Some("Flower Grove".into()),
    };
    app.on_response(
        Response::Listed(SourceId::PrincetonianMini, Ok(vec![item])),
        Instant::now(),
    );
    let text = screen(&app, 80, 24);
    assert!(text.contains("Daily Princetonian Mini"));
    assert!(text.contains("NYT Mini"));
    assert!(text.contains("Fri Apr 3, 2026 · Flower Grove"));

    app.on_response(
        Response::Listed(SourceId::NytMini, Err(SourceError::NeedsCookie)),
        Instant::now(),
    );
    press(&mut app, "l");
    let text = screen(&app, 80, 24);
    assert!(text.contains("NYT_S"), "{text}");
}

#[test]
fn boxed_grid_puts_numbers_in_the_borders() {
    let app = solving(small_data());
    let lines = render(&app, 100, 30);
    let text = lines.join("\n");
    // Row 0's top border carries clue numbers 1, 2 and 3.
    assert!(lines[1].contains("┌1──┬2──┬3──┐"), "{text}");
    // The circled square draws parentheses.
    assert!(text.contains("( )"), "{text}");
    assert!(text.contains("Across") && text.contains("Down"));
    assert!(text.contains("Golden State, briefly"));
    assert!(text.contains("NORMAL"));
    assert!(text.contains("by Ada Lovelace"));
    assert!(text.contains("⏱ 0:00"));
}

#[test]
fn typed_letters_and_insert_mode_show() {
    let mut app = solving(small_data());
    press(&mut app, "icat");
    let text = screen(&app, 100, 30);
    assert!(text.contains("INSERT"));
    assert!(text.contains(" C │ A │ T "), "{text}");
    assert!(text.contains("3/8"), "progress count:\n{text}");
}

#[test]
fn command_line_replaces_the_mode_line() {
    let mut app = solving(small_data());
    press(&mut app, ":check");
    let lines = render(&app, 100, 30);
    assert!(lines.last().unwrap().starts_with(":check"));
}

#[test]
fn daily_size_falls_back_to_compact_on_small_terminals() {
    let app = solving(big_data());
    let roomy = screen(&app, 120, 40);
    assert!(roomy.contains("┌1──┬2──"), "boxed at 120x40");
    let small = screen(&app, 80, 24);
    assert!(!small.contains('┼'), "compact at 80x24:\n{small}");
    assert!(
        small.contains(" ·  ·  · "),
        "blank squares show a dot:\n{small}"
    );
}

#[test]
fn help_overlay_lists_keys_and_scrolls_when_short() {
    let mut app = solving(small_data());
    press(&mut app, "?");
    let wide = screen(&app, 120, 40);
    assert!(
        wide.contains("Normal mode") && wide.contains(":check"),
        "{wide}"
    );
    assert!(wide.contains("any key closes"));

    // At 80x24 the help is one column and scrolls to reach the commands.
    let narrow = screen(&app, 80, 24);
    assert!(narrow.contains("Normal mode") && !narrow.contains(":check"));
    assert!(narrow.contains("j/k scroll"));
    press(&mut app, &"j".repeat(40));
    let scrolled = screen(&app, 80, 24);
    assert!(scrolled.contains(":check"), "{scrolled}");
}

#[test]
fn tiny_terminals_do_not_panic() {
    let mut app = solving(big_data());
    for (w, h) in [(1, 1), (10, 4), (30, 8)] {
        render(&app, w, h);
    }
    press(&mut app, "?");
    render(&app, 10, 4);
    let home = App::new(None);
    render(&home, 5, 3);
}
