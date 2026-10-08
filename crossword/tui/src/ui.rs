//! Ratatui rendering. Drawing is a pure function of the [`App`] state and the
//! current time, which the solve timer needs.
//!
//! The grid has two layouts. The boxed layout draws each square as a 3×1
//! interior inside box-drawing lines, with the clue number set into the
//! square's top border, so a 15×15 needs 61×31 cells. When that does not fit,
//! the compact layout drops the lines and the numbers, which takes 45×15.
//! When even that does not fit, the compact grid scrolls with the cursor.

use std::time::Instant;

use ratatui::Frame;
use ratatui::buffer::Buffer;
use ratatui::layout::{Alignment, Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{
    Block, BorderType, Borders, Clear, List, ListItem, ListState, Paragraph, Tabs, Wrap,
};

use crossword_core::app::{App, Browse, Listing, Mode, Screen, Solve, Tone, format_duration};
use crossword_core::game::{Game, Mark};
use crossword_core::help::{self, Section};
use crossword_core::puzzle::Direction;
use crossword_core::sources::Size;
use crossword_core::store::Status;

const ACCENT: Color = Color::Cyan;
const PAPER: Color = Color::Gray;
const INK: Color = Color::Black;
const BLOCK: Color = Color::Black;
const WORD: Color = Color::LightBlue;
const CURSOR_NORMAL: Color = Color::LightYellow;
const CURSOR_INSERT: Color = Color::LightGreen;
const LINES: Color = Color::DarkGray;
// Letters sit on light squares, so marks use fixed dark shades from the
// 256-colour palette. The 16 named colours follow the terminal theme, and
// dark themes make them too pale to read on paper.
const WRONG: Color = Color::Indexed(160);
const REVEALED: Color = Color::Indexed(90);
const CORRECT: Color = Color::Indexed(28);

/// Narrowest clue panel worth showing beside the grid.
const MIN_CLUE_PANEL: u16 = 24;
/// From this width the Across and Down lists sit side by side.
const SIDE_BY_SIDE: u16 = 52;

/// Draws the whole UI for one frame.
pub fn render(frame: &mut Frame, app: &App, now: Instant) {
    match app.screen {
        Screen::Home => render_home(frame, app),
        Screen::Browse => render_browse(frame, app),
        Screen::Solve => render_solve(frame, app, now),
    }
    if app.show_help {
        render_help(frame, app.help_scroll);
    }
}

fn centered(area: Rect, width: u16, height: u16) -> Rect {
    let width = width.min(area.width);
    let height = height.min(area.height);
    Rect {
        x: area.x + (area.width - width) / 2,
        y: area.y + (area.height - height) / 2,
        width,
        height,
    }
}

fn message_line(app: &App) -> Line<'static> {
    match &app.message {
        Some(m) => Line::from(Span::styled(format!(" {}", m.text), tone_style(m.tone))),
        None => Line::default(),
    }
}

fn tone_style(tone: Tone) -> Style {
    match tone {
        Tone::Info => Style::default(),
        Tone::Success => Style::default()
            .fg(Color::Green)
            .add_modifier(Modifier::BOLD),
        Tone::Error => Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
    }
}

fn key_hints(hints: &[(&str, &str)]) -> Line<'static> {
    let mut spans = vec![Span::raw(" ")];
    for (i, (key, what)) in hints.iter().enumerate() {
        if i > 0 {
            spans.push(Span::styled(" · ", Style::default().fg(LINES)));
        }
        spans.push(Span::styled(key.to_string(), Style::default().fg(ACCENT)));
        spans.push(Span::raw(format!(" {what}")));
    }
    spans.push(Span::raw(" "));
    Line::from(spans)
}

// ----- home ------------------------------------------------------------------

fn render_home(frame: &mut Frame, app: &App) {
    let area = centered(frame.area(), 68, 13);
    let block = Block::bordered()
        .border_type(BorderType::Rounded)
        .title(Span::styled(
            " crossword ",
            Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
        ))
        .title_bottom(key_hints(&[
            ("j/k", "move"),
            ("⏎", "open"),
            ("?", "help"),
            ("q", "quit"),
        ]));
    let inner = block.inner(area);
    frame.render_widget(block, area);

    let mut lines = vec![Line::default()];
    for (i, size) in Size::ALL.iter().enumerate() {
        let selected = i == app.home_selected;
        let marker = if selected { " ▶ " } else { "   " };
        let name_style = if selected {
            Style::default()
                .fg(INK)
                .bg(ACCENT)
                .add_modifier(Modifier::BOLD)
        } else {
            Style::default().add_modifier(Modifier::BOLD)
        };
        lines.push(Line::from(vec![
            Span::styled(marker, Style::default().fg(ACCENT)),
            Span::styled(format!(" {:<10}", size.name()), name_style),
            Span::raw(format!("  {}", size.blurb())),
        ]));
        let sources: Vec<&str> = size.sources().iter().map(|s| s.name()).collect();
        lines.push(Line::from(Span::styled(
            format!("{:16}{}", "", sources.join(" · ")),
            Style::default().fg(LINES),
        )));
        lines.push(Line::default());
    }
    lines.push(message_line(app));
    frame.render_widget(Paragraph::new(lines), inner);
}

// ----- browse ----------------------------------------------------------------

fn status_mark(status: Status) -> Span<'static> {
    match status {
        Status::Solved => Span::styled(
            " ✓ ",
            Style::default()
                .fg(Color::Green)
                .add_modifier(Modifier::BOLD),
        ),
        Status::Started => Span::styled(" ◐ ", Style::default().fg(Color::Yellow)),
        Status::New => Span::raw("   "),
    }
}

fn render_browse(frame: &mut Frame, app: &App) {
    let Some(browse) = app.browse.as_ref() else {
        return;
    };
    let [main, status] =
        Layout::vertical([Constraint::Min(5), Constraint::Length(1)]).areas(frame.area());
    let block = Block::bordered()
        .border_type(BorderType::Rounded)
        .title(Span::styled(
            format!(" {} ", browse.size.name()),
            Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
        ))
        .title_bottom(key_hints(&[
            ("h/l", "source"),
            ("j/k", "move"),
            ("⏎", "open"),
            ("r", "reload"),
            ("q", "back"),
            ("?", "help"),
        ]));
    let inner = block.inner(main);
    frame.render_widget(block, main);

    let [tabs_area, rule, list_area] = Layout::vertical([
        Constraint::Length(1),
        Constraint::Length(1),
        Constraint::Min(1),
    ])
    .areas(inner);
    let titles: Vec<Line> = browse
        .tabs
        .iter()
        .map(|t| Line::from(format!(" {} ", t.source.name())))
        .collect();
    let tabs = Tabs::new(titles)
        .select(browse.active)
        .highlight_style(
            Style::default()
                .fg(INK)
                .bg(ACCENT)
                .add_modifier(Modifier::BOLD),
        )
        .divider(Span::styled("│", Style::default().fg(LINES)));
    frame.render_widget(tabs, tabs_area);
    frame.render_widget(
        Paragraph::new("─".repeat(rule.width as usize)).style(Style::default().fg(LINES)),
        rule,
    );
    render_listing(frame, browse, list_area);
    frame.render_widget(Paragraph::new(message_line(app)), status);
}

fn render_listing(frame: &mut Frame, browse: &Browse, area: Rect) {
    let tab = browse.tab();
    match &tab.listing {
        Listing::Loading => {
            frame.render_widget(
                Paragraph::new("Loading…")
                    .alignment(Alignment::Center)
                    .style(Style::default().fg(LINES)),
                centered(area, area.width, 1),
            );
        }
        Listing::Failed(error) => {
            let text = vec![
                Line::from(Span::styled(error.clone(), Style::default().fg(Color::Red))),
                Line::default(),
                Line::from(Span::styled(
                    "Press r to try again.",
                    Style::default().fg(LINES),
                )),
            ];
            frame.render_widget(
                Paragraph::new(text)
                    .alignment(Alignment::Center)
                    .wrap(Wrap { trim: true }),
                centered(area, area.width.saturating_sub(4), 6),
            );
        }
        Listing::Ready(items) if items.is_empty() => {
            frame.render_widget(
                Paragraph::new("This source lists no puzzles.").alignment(Alignment::Center),
                centered(area, area.width, 1),
            );
        }
        Listing::Ready(items) => {
            let rows: Vec<ListItem> = items
                .iter()
                .map(|item| {
                    ListItem::new(Line::from(vec![
                        status_mark(item.status),
                        Span::raw(item.puzzle.label()),
                    ]))
                })
                .collect();
            let visible = area.height as usize;
            let offset = tab
                .selected
                .saturating_sub(visible / 2)
                .min(items.len().saturating_sub(visible));
            let mut state = ListState::default()
                .with_selected(Some(tab.selected))
                .with_offset(offset);
            let list = List::new(rows)
                .highlight_symbol("▶")
                .highlight_style(Style::default().fg(INK).bg(ACCENT));
            frame.render_stateful_widget(list, area, &mut state);
        }
    }
}

// ----- solve -----------------------------------------------------------------

/// Which grid layout fits.
#[derive(Clone, Copy, PartialEq, Eq)]
enum GridLayout {
    Boxed,
    Compact,
}

impl GridLayout {
    fn size(self, game: &Game) -> (u16, u16) {
        let (w, h) = (game.puzzle().width() as u16, game.puzzle().height() as u16);
        match self {
            GridLayout::Boxed => (w * 4 + 1, h * 2 + 1),
            GridLayout::Compact => (w * 3, h),
        }
    }
}

fn render_solve(frame: &mut Frame, app: &App, now: Instant) {
    let Some(solve) = app.solve.as_ref() else {
        return;
    };
    let game = &solve.game;
    let [header, body, clue_bar, mode_line] = Layout::vertical([
        Constraint::Length(1),
        Constraint::Min(3),
        Constraint::Length(4),
        Constraint::Length(1),
    ])
    .areas(frame.area());

    render_header(frame, solve, header, now);

    let fits = |(w, h): (u16, u16)| w <= body.width && h <= body.height;
    let layout = if fits(GridLayout::Boxed.size(game)) {
        GridLayout::Boxed
    } else {
        GridLayout::Compact
    };
    let (grid_w, grid_h) = layout.size(game);
    let panel_w = body.width.saturating_sub(grid_w + 2);
    let grid_area = if panel_w >= MIN_CLUE_PANEL {
        let panel = Rect {
            x: body.x + grid_w + 2,
            width: panel_w,
            ..body
        };
        render_clue_panel(frame, game, panel);
        Rect {
            x: body.x + 1,
            y: body.y,
            width: grid_w.min(body.width),
            height: grid_h.min(body.height),
        }
    } else {
        centered(body, grid_w, grid_h)
    };
    let cursor = match solve.mode {
        Mode::Insert => CURSOR_INSERT,
        _ => CURSOR_NORMAL,
    };
    draw_grid(frame.buffer_mut(), grid_area, game, layout, cursor);

    render_clue_bar(frame, game, clue_bar);
    render_mode_line(frame, app, solve, mode_line);
}

fn render_header(frame: &mut Frame, solve: &Solve, area: Rect, now: Instant) {
    let game = &solve.game;
    let meta = game.puzzle().meta();
    let mut left = vec![Span::styled(
        format!(" {} ", solve.puzzle.source.name()),
        Style::default()
            .fg(INK)
            .bg(ACCENT)
            .add_modifier(Modifier::BOLD),
    )];
    let date = meta
        .date
        .clone()
        .or_else(|| solve.puzzle.date.map(|d| d.to_string()));
    let mut details: Vec<String> = Vec::new();
    if let Some(date) = date {
        details.push(date);
    }
    if !meta.title.is_empty() {
        details.push(meta.title.clone());
    }
    if let Some(byline) = meta.byline() {
        details.push(byline);
    }
    left.push(Span::styled(
        format!(" {}", details.join(" · ")),
        Style::default().add_modifier(Modifier::BOLD),
    ));

    let clock = format_duration(game.elapsed(now));
    let right = if game.is_solved() {
        Span::styled(
            format!(" ✓ {clock} "),
            Style::default()
                .fg(Color::Green)
                .add_modifier(Modifier::BOLD),
        )
    } else {
        Span::styled(format!(" ⏱ {clock} "), Style::default().fg(ACCENT))
    };
    let right_w = right.width() as u16;
    frame.render_widget(
        Paragraph::new(Line::from(left)),
        Rect {
            width: area.width.saturating_sub(right_w),
            ..area
        },
    );
    frame.render_widget(
        Paragraph::new(Line::from(right)).alignment(Alignment::Right),
        area,
    );
}

/// Writes `text` at (x, y) when the row is inside `area`, clipped to its width.
fn put(buf: &mut Buffer, area: Rect, x: u16, y: u16, text: &str, style: Style) {
    if y >= area.bottom() || x >= area.right() {
        return;
    }
    buf.set_stringn(x, y, text, (area.right() - x) as usize, style);
}

/// The background of an open square: the cursor, the current word or paper.
fn square_bg(game: &Game, cell: usize, word: &[usize], cursor_bg: Color) -> Color {
    if cell == game.cursor() {
        cursor_bg
    } else if word.contains(&cell) {
        WORD
    } else {
        PAPER
    }
}

/// The style and three-character interior of one open square: the letter,
/// with `( )` around it in a circled square and a `+` after a rebus.
fn square(game: &Game, cell: usize, word: &[usize], cursor_bg: Color) -> (String, Style) {
    let letter = game.letter(cell);
    let mut chars = letter.chars();
    let first = chars.next().unwrap_or(' ');
    let rebus = chars.next().is_some();
    let (open, close) = if game.puzzle().is_circled(cell) {
        ('(', ')')
    } else {
        (' ', ' ')
    };
    let close = if rebus { '+' } else { close };
    // Wrong letters are also struck through, so the mark does not rely on
    // colour alone.
    let (fg, extra) = match game.mark(cell) {
        Mark::Wrong => (WRONG, Modifier::CROSSED_OUT),
        Mark::Revealed => (REVEALED, Modifier::ITALIC),
        Mark::Correct => (CORRECT, Modifier::empty()),
        Mark::None => (INK, Modifier::empty()),
    };
    let bg = square_bg(game, cell, word, cursor_bg);
    (
        format!("{open}{first}{close}"),
        Style::default()
            .fg(fg)
            .bg(bg)
            .add_modifier(Modifier::BOLD | extra),
    )
}

fn draw_grid(buf: &mut Buffer, area: Rect, game: &Game, layout: GridLayout, cursor_bg: Color) {
    let puzzle = game.puzzle();
    let (w, h) = (puzzle.width(), puzzle.height());
    let word = puzzle.entry(game.current_entry()).cells.clone();

    if layout == GridLayout::Compact {
        // A grid larger than the area shows the part around the cursor.
        let first_row = scroll_start(game.cursor() / w, area.height as usize, h);
        let first_col = scroll_start(game.cursor() % w, area.width as usize / 3, w);
        for row in first_row..h {
            for col in first_col..w {
                let cell = row * w + col;
                let (x, y) = (
                    area.x + (col - first_col) as u16 * 3,
                    area.y + (row - first_row) as u16,
                );
                if puzzle.is_open(cell) {
                    let (text, style) = square(game, cell, &word, cursor_bg);
                    let text = if text.trim().is_empty() {
                        " · ".to_string()
                    } else {
                        text
                    };
                    put(buf, area, x, y, &text, style);
                } else {
                    put(buf, area, x, y, "   ", Style::default().bg(BLOCK));
                }
            }
        }
        return;
    }

    // Each square is the three columns of its top rule plus the three of its
    // letter row, so it reads as a 3x2 block. The rule carries the number.
    // The corner and the bar between two squares of a row sit on paper when
    // both squares are open. Inside an across word they take the word
    // colour, so the word reads as one bar.
    let cell_at = |row: isize, col: isize| -> Option<usize> {
        (row >= 0 && col >= 0 && (row as usize) < h && (col as usize) < w)
            .then(|| row as usize * w + col as usize)
    };
    let open = |cell: Option<usize>| cell.is_some_and(|c| puzzle.is_open(c));
    let in_word = |cell: Option<usize>| cell.is_some_and(|c| word.contains(&c));
    let line = |bg: Option<Color>| {
        let style = Style::default().fg(LINES);
        match bg {
            Some(bg) => style.bg(bg),
            None => style,
        }
    };
    let across_word = puzzle.entry(game.current_entry()).direction == Direction::Across;

    for row in 0..=h as isize {
        let y = area.y + row as u16 * 2;
        let (top, bottom) = (row == 0, row == h as isize);
        for col in 0..=w as isize {
            let x = area.x + col as u16 * 4;
            let (left, right) = (col == 0, col == w as isize);
            let corner = match (top, bottom, left, right) {
                (true, _, true, _) => "┌",
                (true, _, _, true) => "┐",
                (true, ..) => "┬",
                (_, true, true, _) => "└",
                (_, true, _, true) => "┘",
                (_, true, ..) => "┴",
                (.., true, _) => "├",
                (.., true) => "┤",
                _ => "┼",
            };
            let (below_l, below_r) = (cell_at(row, col - 1), cell_at(row, col));
            let corner_bg = if across_word && in_word(below_l) && in_word(below_r) {
                Some(WORD)
            } else if open(below_l) && open(below_r) {
                Some(PAPER)
            } else {
                None
            };
            put(buf, area, x, y, corner, line(corner_bg));

            // The top rule of the square below, in that square's colour.
            if let Some(cell) = below_r {
                let x = x + 1;
                if puzzle.is_open(cell) {
                    let bg = square_bg(game, cell, &word, cursor_bg);
                    put(buf, area, x, y, "───", line(Some(bg)));
                    if let Some(number) = puzzle.number(cell) {
                        put(
                            buf,
                            area,
                            x,
                            y,
                            &number.to_string(),
                            Style::default().fg(INK).bg(bg),
                        );
                    }
                } else {
                    put(buf, area, x, y, "───", line(None));
                }
            } else if !right {
                put(buf, area, x + 1, y, "───", line(None));
            }
        }
        if bottom {
            break;
        }

        let y = y + 1;
        for col in 0..=w as isize {
            let x = area.x + col as u16 * 4;
            let (l, r) = (cell_at(row, col - 1), cell_at(row, col));
            let bar_bg = if across_word && in_word(l) && in_word(r) {
                Some(WORD)
            } else if open(l) && open(r) {
                Some(PAPER)
            } else {
                None
            };
            put(buf, area, x, y, "│", line(bar_bg));
            if let Some(cell) = r {
                if puzzle.is_open(cell) {
                    let (text, style) = square(game, cell, &word, cursor_bg);
                    put(buf, area, x + 1, y, &text, style);
                } else {
                    put(buf, area, x + 1, y, "   ", Style::default().bg(BLOCK));
                }
            }
        }
    }
}

/// The first of `total` rows or columns to draw when only `shown` fit. It
/// keeps `pos` in view, in the middle where the edges allow.
fn scroll_start(pos: usize, shown: usize, total: usize) -> usize {
    if shown == 0 || shown >= total {
        return 0;
    }
    pos.saturating_sub(shown / 2).min(total - shown)
}

/// Greedy word wrap to `width` columns.
fn wrap(text: &str, width: usize) -> Vec<String> {
    let width = width.max(1);
    let mut lines = Vec::new();
    let mut current = String::new();
    for word in text.split_whitespace() {
        let mut word = word.to_string();
        loop {
            let used = current.chars().count();
            let len = word.chars().count();
            if used == 0 && len > width {
                // A word longer than the line is split.
                let head: String = word.chars().take(width).collect();
                word = word.chars().skip(width).collect();
                lines.push(head);
                continue;
            }
            if used > 0 && used + 1 + len > width {
                lines.push(std::mem::take(&mut current));
                continue;
            }
            if used > 0 {
                current.push(' ');
            }
            current.push_str(&word);
            break;
        }
    }
    if !current.is_empty() || lines.is_empty() {
        lines.push(current);
    }
    lines
}

fn render_clue_panel(frame: &mut Frame, game: &Game, area: Rect) {
    let areas: [Rect; 2] = if area.width >= SIDE_BY_SIDE {
        Layout::horizontal([Constraint::Ratio(1, 2), Constraint::Ratio(1, 2)]).areas(area)
    } else {
        Layout::vertical([Constraint::Ratio(1, 2), Constraint::Ratio(1, 2)]).areas(area)
    };
    for (direction, area) in [(Direction::Across, areas[0]), (Direction::Down, areas[1])] {
        render_clue_list(frame, game, direction, area);
    }
}

fn render_clue_list(frame: &mut Frame, game: &Game, direction: Direction, area: Rect) {
    let active = game.current_entry();
    let crossing = game.crossing_entry();
    let focused = game.puzzle().entry(active).direction == direction;
    let title_style = if focused {
        Style::default().fg(ACCENT).add_modifier(Modifier::BOLD)
    } else {
        Style::default().add_modifier(Modifier::BOLD)
    };
    let block = Block::new()
        .borders(Borders::TOP)
        .border_style(Style::default().fg(LINES))
        .title(Span::styled(format!(" {} ", direction.name()), title_style));
    let inner = block.inner(area);
    frame.render_widget(block, area);
    if inner.width < 6 || inner.height == 0 {
        return;
    }

    let text_w = inner.width as usize - 4;
    let mut lines: Vec<Line> = Vec::new();
    let mut anchor = 0;
    for (index, entry) in game.puzzle().entries().iter().enumerate() {
        if entry.direction != direction {
            continue;
        }
        let style = if index == active {
            Style::default().fg(INK).bg(WORD)
        } else if Some(index) == crossing {
            Style::default().add_modifier(Modifier::BOLD)
        } else if game.entry_is_full(index) {
            Style::default().fg(LINES)
        } else {
            Style::default()
        };
        if index == active || (Some(index) == crossing && !focused) {
            anchor = lines.len();
        }
        let clue = if entry.clue.is_empty() {
            "(no clue)"
        } else {
            &entry.clue
        };
        for (i, part) in wrap(clue, text_w).into_iter().enumerate() {
            let prefix = if i == 0 {
                format!("{:>3} ", entry.number)
            } else {
                "    ".to_string()
            };
            let pad = text_w.saturating_sub(part.chars().count());
            lines.push(Line::from(Span::styled(
                format!("{prefix}{part}{}", " ".repeat(pad)),
                style,
            )));
        }
    }
    let visible = inner.height as usize;
    let offset = anchor
        .saturating_sub(visible / 3)
        .min(lines.len().saturating_sub(visible));
    frame.render_widget(Paragraph::new(lines).scroll((offset as u16, 0)), inner);
}

fn render_clue_bar(frame: &mut Frame, game: &Game, area: Rect) {
    let entry = game.puzzle().entry(game.current_entry());
    let block = Block::bordered()
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(LINES))
        .title(Span::styled(
            format!(" {}{} ", entry.number, entry.direction.letter()),
            Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
        ));
    let mut lines = vec![Line::from(Span::styled(
        if entry.clue.is_empty() {
            "(no clue)".to_string()
        } else {
            entry.clue.clone()
        },
        Style::default().add_modifier(Modifier::BOLD),
    ))];
    if let Some(crossing) = game.crossing_entry() {
        let other = game.puzzle().entry(crossing);
        lines.push(Line::from(Span::styled(
            format!(
                "{}{}  {}",
                other.number,
                other.direction.letter(),
                other.clue
            ),
            Style::default().fg(LINES),
        )));
    }
    frame.render_widget(
        Paragraph::new(lines).block(block).wrap(Wrap { trim: true }),
        area,
    );
}

fn render_mode_line(frame: &mut Frame, app: &App, solve: &Solve, area: Rect) {
    let game = &solve.game;
    match solve.mode {
        Mode::Command => {
            let text = format!(":{}", solve.input);
            let width = text.chars().count() as u16;
            frame.render_widget(Paragraph::new(text), area);
            frame.set_cursor_position((area.x + width.min(area.width.saturating_sub(1)), area.y));
            return;
        }
        Mode::Rebus => {
            let label = " REBUS ";
            let line = Line::from(vec![
                Span::styled(
                    label,
                    Style::default()
                        .fg(INK)
                        .bg(Color::Magenta)
                        .add_modifier(Modifier::BOLD),
                ),
                Span::raw(format!(" {}", solve.input)),
                Span::styled(
                    "   ⏎ fill the square · Esc cancel",
                    Style::default().fg(LINES),
                ),
            ]);
            frame.render_widget(Paragraph::new(line), area);
            let x = area.x + label.len() as u16 + 1 + solve.input.chars().count() as u16;
            frame.set_cursor_position((x.min(area.right().saturating_sub(1)), area.y));
            return;
        }
        _ => {}
    }

    let (label, color) = match solve.mode {
        Mode::Insert => (" INSERT ", Color::Green),
        _ => (" NORMAL ", ACCENT),
    };
    let mut left = vec![Span::styled(
        label,
        Style::default()
            .fg(INK)
            .bg(color)
            .add_modifier(Modifier::BOLD),
    )];
    left.extend(message_line(app).spans);

    let (filled, open) = game.fill_counts();
    let right = Line::from(vec![
        Span::styled(
            format!("{}  {filled}/{open} ", solve.showcmd()),
            Style::default().fg(LINES),
        ),
        Span::styled("?", Style::default().fg(ACCENT)),
        Span::raw(" help "),
    ]);
    let right_w = right.width() as u16;
    frame.render_widget(
        Paragraph::new(Line::from(left)),
        Rect {
            width: area.width.saturating_sub(right_w),
            ..area
        },
    );
    frame.render_widget(Paragraph::new(right).alignment(Alignment::Right), area);
}

// ----- help ------------------------------------------------------------------

/// Width of the key column in each help column.
const HELP_KEYS: usize = 17;
/// Inner width of the two-column help: 2 + 17 + 36 on the left, a gap of
/// 2, and 2 + 17 + 28 on the right.
const HELP_WIDE: u16 = 104;

fn help_section(section: &Section) -> Vec<Line<'static>> {
    let mut lines = vec![Line::from(Span::styled(
        section.title,
        Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
    ))];
    for (keys, what) in section.rows {
        lines.push(Line::from(vec![
            Span::styled(
                format!("  {keys:<HELP_KEYS$}"),
                Style::default().add_modifier(Modifier::BOLD),
            ),
            Span::raw(*what),
        ]));
    }
    lines.push(Line::default());
    lines
}

fn help_columns() -> (Vec<Line<'static>>, Vec<Line<'static>>) {
    let column = |sections: &[Section]| sections.iter().flat_map(help_section).collect();
    (column(help::COLUMNS[0]), column(help::COLUMNS[1]))
}

/// Where the help overlay sits on `screen`, whether it shows two columns,
/// and how many lines it holds.
fn help_layout(screen: Rect) -> (Rect, bool, u16) {
    let (left, right) = help_columns();
    let wide = screen.width >= HELP_WIDE + 2;
    let (width, content) = if wide {
        (HELP_WIDE + 2, left.len().max(right.len()) as u16)
    } else {
        (60, (left.len() + right.len()) as u16)
    };
    (centered(screen, width, content + 2), wide, content)
}

/// How far the help overlay can scroll on `screen`. The event loop reports
/// it to the app, so that `j` stops where the view stops.
pub fn help_max_scroll(screen: Rect) -> u16 {
    let (area, _, content) = help_layout(screen);
    content.saturating_sub(area.height.saturating_sub(2))
}

fn render_help(frame: &mut Frame, scroll: u16) {
    let screen = frame.area();
    let (left, right) = help_columns();
    let (area, wide, content) = help_layout(screen);
    let visible = area.height.saturating_sub(2);
    let scroll = scroll.min(help_max_scroll(screen));
    let hint = if content > visible {
        " j/k scroll · other keys close "
    } else {
        " any key closes "
    };
    frame.render_widget(Clear, area);
    let block = Block::bordered()
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(ACCENT))
        .title(Span::styled(
            " keys ",
            Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
        ))
        .title_bottom(Line::from(hint).alignment(Alignment::Right));
    let inner = block.inner(area);
    frame.render_widget(block, area);
    if wide {
        let [l, _, r] = Layout::horizontal([
            Constraint::Length(55),
            Constraint::Length(2),
            Constraint::Min(10),
        ])
        .areas(inner);
        frame.render_widget(Paragraph::new(left).scroll((scroll, 0)), l);
        frame.render_widget(Paragraph::new(right).scroll((scroll, 0)), r);
    } else {
        let all = [left, right].concat();
        frame.render_widget(Paragraph::new(all).scroll((scroll, 0)), inner);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scroll_keeps_the_position_in_view() {
        assert_eq!(scroll_start(20, 18, 21), 3); // the last row, at the bottom
        assert_eq!(scroll_start(0, 18, 21), 0);
        assert_eq!(scroll_start(10, 18, 21), 1); // near the middle
        assert_eq!(scroll_start(5, 18, 15), 0); // it all fits
        assert_eq!(scroll_start(5, 0, 15), 0);
    }

    #[test]
    fn wrap_breaks_on_words_and_splits_long_ones() {
        assert_eq!(wrap("one two three", 7), ["one two", "three"]);
        assert_eq!(wrap("abcdefghij", 4), ["abcd", "efgh", "ij"]);
        assert_eq!(wrap("", 5), [""]);
    }
}
