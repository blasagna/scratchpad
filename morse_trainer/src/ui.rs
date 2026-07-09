//! Ratatui rendering of the [`App`] state. Rendering is a pure function of the
//! state plus a preformatted legend line describing the active key bindings.

use ratatui::Frame;
use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, List, ListItem, Paragraph, Wrap};

use crate::app::{App, Mode, Screen};
use crate::lessons;
use crate::morse::{self, Symbol};

/// Renders the whole UI for the current frame.
pub fn render(frame: &mut Frame, app: &App, legend: &str) {
    match app.screen {
        Screen::Menu => render_menu(frame, app),
        Screen::LessonPicker => render_lesson_picker(frame, app),
        Screen::Practice => render_practice(frame, app, legend),
    }
}

fn render_menu(frame: &mut Frame, app: &App) {
    let items = ["Free text entry", "Prompted practice (lessons & phrases)"];
    render_selection_screen(
        frame,
        " morse trainer ",
        "Choose a mode:",
        &items,
        app.selection,
        "↑/↓ move   ⏎ select   q quit",
    );
}

fn render_lesson_picker(frame: &mut Frame, app: &App) {
    let mut items: Vec<String> = vec!["Phrases (mixed)".to_string()];
    for lesson in lessons::LESSONS {
        items.push(format!("Lesson {}", lesson.title));
    }
    let refs: Vec<&str> = items.iter().map(String::as_str).collect();
    render_selection_screen(
        frame,
        " choose a track ",
        "Prompted practice:",
        &refs,
        app.selection,
        "↑/↓ move   ⏎ select   esc back   q quit",
    );
}

/// Shared renderer for the two menu-style screens.
fn render_selection_screen(
    frame: &mut Frame,
    title: &str,
    heading: &str,
    items: &[&str],
    selected: usize,
    footer: &str,
) {
    let area = centered_rect(60, 60, frame.area());
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(2),
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(area);

    let header = Paragraph::new(heading).style(Style::default().add_modifier(Modifier::BOLD));
    frame.render_widget(header, chunks[0]);

    let list_items: Vec<ListItem> = items
        .iter()
        .enumerate()
        .map(|(i, label)| {
            let marker = if i == selected { "▶ " } else { "  " };
            let style = if i == selected {
                Style::default().fg(Color::Black).bg(Color::Cyan)
            } else {
                Style::default()
            };
            ListItem::new(Line::from(format!("{marker}{label}"))).style(style)
        })
        .collect();
    let list = List::new(list_items).block(Block::default().borders(Borders::ALL).title(title));
    frame.render_widget(list, chunks[1]);

    let help = Paragraph::new(footer).style(Style::default().fg(Color::DarkGray));
    frame.render_widget(help, chunks[2]);
}

fn render_practice(frame: &mut Frame, app: &App, legend: &str) {
    let area = frame.area();
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3), // legend
            Constraint::Min(4),    // content
            Constraint::Length(3), // keying + help
        ])
        .split(area);

    let legend_widget = Paragraph::new(legend)
        .block(Block::default().borders(Borders::ALL).title(" keys "))
        .style(Style::default().fg(Color::DarkGray));
    frame.render_widget(legend_widget, chunks[0]);

    match app.mode {
        Mode::FreeText => render_free_text(frame, app, chunks[1]),
        Mode::Prompted => render_prompted(frame, app, chunks[1]),
    }

    render_keying_bar(frame, app, chunks[2]);
}

fn render_free_text(frame: &mut Frame, app: &App, area: Rect) {
    let text = if app.output.is_empty() {
        Line::from(Span::styled(
            "(start keying — decoded text appears here)",
            Style::default().fg(Color::DarkGray),
        ))
    } else {
        Line::from(app.output.as_str())
    };
    let widget = Paragraph::new(text)
        .block(Block::default().borders(Borders::ALL).title(" decoded "))
        .wrap(Wrap { trim: false });
    frame.render_widget(widget, area);
}

fn render_prompted(frame: &mut Frame, app: &App, area: Rect) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3), // target
            Constraint::Length(3), // typed
            Constraint::Min(1),    // hint / progress
        ])
        .split(area);

    let target = Paragraph::new(Line::from(target_spans(app)))
        .block(Block::default().borders(Borders::ALL).title(" target "))
        .wrap(Wrap { trim: false });
    frame.render_widget(target, chunks[0]);

    let typed_display = visible_spaces(&app.typed);
    let typed = Paragraph::new(typed_display)
        .block(Block::default().borders(Borders::ALL).title(" your input "))
        .wrap(Wrap { trim: false });
    frame.render_widget(typed, chunks[1]);

    let mut hint_lines = vec![Line::from(format!(
        "prompt {}   ·   tab: toggle hint",
        app.prompt_index + 1
    ))];
    if app.show_hint {
        match app.next_expected_char().and_then(morse::encode) {
            Some(seq) => {
                let ch = app.next_expected_char().unwrap();
                let shown = if ch == ' ' {
                    "space".to_string()
                } else {
                    ch.to_string()
                };
                hint_lines.push(Line::from(Span::styled(
                    format!("next: {}  =  {}", shown, morse::render_sequence(seq)),
                    Style::default().fg(Color::Yellow),
                )));
            }
            None => hint_lines.push(Line::from(Span::styled(
                "prompt complete!",
                Style::default().fg(Color::Green),
            ))),
        }
    }
    let hint = Paragraph::new(hint_lines).style(Style::default().fg(Color::DarkGray));
    frame.render_widget(hint, chunks[2]);
}

/// Builds the target line with per-character coloring: green for correctly
/// reproduced characters, red for mistakes, yellow underline for the current
/// position, and dim gray for characters not yet reached.
fn target_spans(app: &App) -> Vec<Span<'static>> {
    let typed: Vec<char> = app.typed.chars().collect();
    app.target
        .chars()
        .enumerate()
        .map(|(i, tc)| {
            let display = if tc == ' ' { '␣' } else { tc };
            let style = if i < typed.len() {
                if typed[i] == tc {
                    Style::default().fg(Color::Green)
                } else {
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)
                }
            } else if i == typed.len() {
                Style::default()
                    .fg(Color::Yellow)
                    .add_modifier(Modifier::UNDERLINED | Modifier::BOLD)
            } else {
                Style::default().fg(Color::DarkGray)
            };
            Span::styled(display.to_string(), style)
        })
        .collect()
}

fn visible_spaces(s: &str) -> String {
    s.replace(' ', "␣")
}

fn render_keying_bar(frame: &mut Frame, app: &App, area: Rect) {
    let pending = app.pending();
    let content = if pending.is_empty() {
        Span::styled("(waiting)", Style::default().fg(Color::DarkGray))
    } else {
        Span::styled(
            morse::render_sequence(pending),
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        )
    };
    let line = Line::from(vec![
        Span::raw("keying: "),
        content,
        Span::styled(
            format!(
                "      space: {}   backspace: {}",
                extension_sequence(Symbol::Char(' ')),
                extension_sequence(Symbol::Backspace),
            ),
            Style::default().fg(Color::DarkGray),
        ),
    ]);
    let widget = Paragraph::new(line)
        .block(Block::default().borders(Borders::ALL).title(" input "))
        .alignment(Alignment::Left);
    frame.render_widget(widget, area);
}

/// The dot/dash rendering of an extension symbol, e.g. space -> `..--`.
fn extension_sequence(symbol: Symbol) -> String {
    morse::encode_symbol(symbol)
        .map(morse::render_sequence)
        .unwrap_or_default()
}

/// Centers a rectangle taking `percent_x` × `percent_y` of `r`.
fn centered_rect(percent_x: u16, percent_y: u16, r: Rect) -> Rect {
    let vertical = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Percentage((100 - percent_y) / 2),
            Constraint::Percentage(percent_y),
            Constraint::Percentage((100 - percent_y) / 2),
        ])
        .split(r);
    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage((100 - percent_x) / 2),
            Constraint::Percentage(percent_x),
            Constraint::Percentage((100 - percent_x) / 2),
        ])
        .split(vertical[1])[1]
}
