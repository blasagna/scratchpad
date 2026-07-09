//! Renders each screen through a headless `TestBackend` to confirm the UI draws
//! without panicking and shows the expected content. This is the automated
//! stand-in for a manual TUI smoke test.

use morse_trainer::app::App;
use morse_trainer::morse::InputEvent::Dot;
use morse_trainer::ui;
use ratatui::Terminal;
use ratatui::backend::TestBackend;

const LEGEND: &str = "dot: j   dash: k   quit: q   timeout: 300ms";

/// Renders `app` to an 80x24 test terminal and returns the buffer as text.
fn render_to_string(app: &App) -> String {
    let backend = TestBackend::new(80, 24);
    let mut terminal = Terminal::new(backend).unwrap();
    terminal
        .draw(|frame| ui::render(frame, app, LEGEND))
        .unwrap();
    let buffer = terminal.backend().buffer().clone();
    buffer.content().iter().map(|cell| cell.symbol()).collect()
}

#[test]
fn menu_screen_renders() {
    let app = App::new();
    let text = render_to_string(&app);
    assert!(text.contains("morse trainer"));
    assert!(text.contains("Free text"));
}

#[test]
fn lesson_picker_renders() {
    let mut app = App::new();
    app.menu_down();
    app.menu_select(); // -> LessonPicker
    let text = render_to_string(&app);
    assert!(text.contains("Phrases"));
    assert!(text.contains("Lesson"));
}

#[test]
fn free_text_practice_renders_decoded_output() {
    let mut app = App::new();
    app.menu_select(); // Free text
    app.push(Dot);
    app.push(Dot);
    app.commit_sequence(); // "i"
    let text = render_to_string(&app);
    assert!(text.contains("decoded"));
    assert!(text.contains('i'));
    assert!(text.contains("keying"));
    // The extension reminders are always visible in the input bar.
    assert!(text.contains("space: ..--"));
    assert!(text.contains("backspace: ----"));
}

#[test]
fn prompted_practice_renders_target_and_hint() {
    let mut app = App::new();
    app.menu_down();
    app.menu_select();
    app.selection = 1; // first lesson ("E T"), target "e"
    app.menu_select();
    app.toggle_hint();
    let text = render_to_string(&app);
    assert!(text.contains("target"));
    assert!(text.contains("your input"));
    // Hint shows the Morse for the next expected char 'e' == "."
    assert!(text.contains("next: e"));
}

#[test]
fn legend_overlay_renders_when_toggled_on() {
    let mut app = App::new();
    app.menu_select(); // Free text practice

    let hidden = render_to_string(&app);
    assert!(!hidden.contains("morse legend"));

    app.toggle_legend();
    let shown = render_to_string(&app);
    assert!(shown.contains("morse legend"));
    // A couple of table entries should appear in the chart.
    assert!(shown.contains("space"));
    assert!(shown.contains("bksp"));
}
