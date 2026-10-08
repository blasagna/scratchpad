//! The GUI's views. Like the TUI's drawing, each view is a pure function of
//! the core [`App`] state; clicks become [`Message`]s for the core.

use std::time::Instant;

use iced::font::{self, Font};
use iced::widget::{
    Column, button, canvas, center, column, container, mouse_area, opaque, row, scrollable, space,
    stack, text,
};
use iced::{Center, Element, Fill, FillPortion};

use crossword_core::app::{App, Listing, Mode, Screen, Solve, Tone, format_duration};
use crossword_core::game::Game;
use crossword_core::help::{self, Section};
use crossword_core::puzzle::Direction;
use crossword_core::sources::Size;
use crossword_core::store::Status;

use crate::grid::Grid;
use crate::style::{self, ACCENT, CORRECT, ClueKind, INK, MUTED, REVEALED, STARTED, WRONG};
use crate::{ACROSS_LIST, DOWN_LIST, Message, PUZZLE_LIST};

const BOLD: Font = Font {
    weight: font::Weight::Bold,
    ..Font::DEFAULT
};

pub fn view(app: &App, now: Instant) -> Element<'_, Message> {
    let screen = match (app.screen, &app.solve) {
        (Screen::Solve, Some(solve)) => solve_view(app, solve, now),
        (Screen::Browse, _) if app.browse.is_some() => browse_view(app),
        _ => home_view(app),
    };
    if app.show_help {
        help_overlay(screen)
    } else {
        screen
    }
}

fn status_line(app: &App) -> Element<'_, Message> {
    match &app.message {
        Some(m) => {
            let colour = match m.tone {
                Tone::Info => INK,
                Tone::Success => CORRECT,
                Tone::Error => WRONG,
            };
            text(&m.text).color(colour).into()
        }
        None => text("").into(),
    }
}

fn hints<'a>(pairs: &[(&'a str, &'a str)]) -> Element<'a, Message> {
    let mut line = row![].spacing(14);
    for &(keys, what) in pairs {
        line = line.push(
            row![
                text(keys).font(Font::MONOSPACE).color(ACCENT),
                text(what).color(MUTED)
            ]
            .spacing(5),
        );
    }
    line.into()
}

fn badge<'a>(label: impl text::IntoFragment<'a>, colour: iced::Color) -> Element<'a, Message> {
    container(text(label).size(13).font(BOLD))
        .padding([3, 8])
        .style(style::badge(colour))
        .into()
}

// ----- home ------------------------------------------------------------------

fn home_view(app: &App) -> Element<'_, Message> {
    let mut sizes = Column::new().spacing(12);
    for (i, &size) in Size::ALL.iter().enumerate() {
        let sources: Vec<&str> = size.sources().iter().map(|s| s.name()).collect();
        let content = column![
            row![
                text(size.name()).size(24).font(BOLD),
                text(size.blurb()).size(15),
            ]
            .spacing(16)
            .align_y(Center),
            text(sources.join(" · ")).size(13),
        ]
        .spacing(4);
        sizes = sizes.push(
            button(content)
                .width(480)
                .padding(16)
                .style(style::choice(i == app.home_selected))
                .on_press(Message::Size(size)),
        );
    }
    let body = column![
        text("crossword").size(44).font(BOLD),
        text("Choose a size").size(16).color(MUTED),
        sizes,
        hints(&[
            ("j/k", "move"),
            ("Enter", "open"),
            ("?", "help"),
            ("q", "quit")
        ]),
        status_line(app),
    ]
    .spacing(20)
    .align_x(Center);
    center(body).into()
}

// ----- browse ----------------------------------------------------------------

fn status_label(status: Status) -> Element<'static, Message> {
    let (label, colour) = match status {
        Status::Solved => ("Solved", CORRECT),
        Status::Started => ("Started", STARTED),
        Status::New => ("", MUTED),
    };
    text(label)
        .size(13)
        .font(BOLD)
        .color(colour)
        .width(64)
        .into()
}

fn browse_view(app: &App) -> Element<'_, Message> {
    let Some(browse) = app.browse.as_ref() else {
        return home_view(app);
    };
    let header = row![
        button(text("← Sizes"))
            .style(style::link)
            .on_press(Message::Back),
        text(browse.size.name()).size(28).font(BOLD),
    ]
    .spacing(16)
    .align_y(Center);

    let mut tabs = row![].spacing(6);
    for (i, tab) in browse.tabs.iter().enumerate() {
        tabs = tabs.push(
            button(text(tab.source.name()))
                .padding([7, 16])
                .style(style::tab(i == browse.active))
                .on_press(Message::Tab(i)),
        );
    }

    let tab = browse.tab();
    let body: Element<Message> = match &tab.listing {
        Listing::Loading => center(text("Loading…").color(MUTED)).into(),
        Listing::Failed(error) => center(
            column![
                text(error).color(WRONG),
                button(text("Try again"))
                    .padding([7, 16])
                    .style(style::tab(true))
                    .on_press(Message::Reload),
            ]
            .spacing(12)
            .align_x(Center)
            .max_width(600),
        )
        .into(),
        Listing::Ready(items) if items.is_empty() => {
            center(text("This source lists no puzzles.")).into()
        }
        Listing::Ready(items) => {
            let rows = items.iter().enumerate().map(|(i, item)| {
                button(
                    row![status_label(item.status), text(item.puzzle.label())]
                        .spacing(8)
                        .align_y(Center),
                )
                .width(Fill)
                .padding([6, 12])
                .style(style::list_item(i == tab.selected))
                .on_press(Message::Item(i))
                .into()
            });
            scrollable(Column::with_children(rows).spacing(2))
                .id(PUZZLE_LIST)
                .height(Fill)
                .into()
        }
    };

    column![
        header,
        tabs,
        body,
        hints(&[
            ("h/l", "source"),
            ("j/k", "move"),
            ("Enter", "open"),
            ("r", "reload"),
            ("q", "back"),
            ("?", "help"),
        ]),
        status_line(app),
    ]
    .spacing(14)
    .padding(24)
    .into()
}

// ----- solve -----------------------------------------------------------------

fn solve_view<'a>(app: &'a App, solve: &'a Solve, now: Instant) -> Element<'a, Message> {
    let game = &solve.game;
    let meta = game.puzzle().meta();
    let mut details: Vec<String> = Vec::new();
    if let Some(date) = meta
        .date
        .clone()
        .or_else(|| solve.puzzle.date.map(|d| d.to_string()))
    {
        details.push(date);
    }
    if !meta.title.is_empty() {
        details.push(meta.title.clone());
    }
    if !meta.author.is_empty() {
        details.push(format!("by {}", meta.author));
    }
    let clock = format_duration(game.elapsed(now));
    let clock = if game.is_solved() {
        text(format!("✓ {clock}")).color(CORRECT)
    } else {
        text(clock)
    };
    let header = row![
        button(text("← List"))
            .style(style::link)
            .on_press(Message::Back),
        badge(solve.puzzle.source.name(), ACCENT),
        text(details.join(" · ")).size(15),
        space::horizontal(),
        clock.size(20).font(BOLD),
    ]
    .spacing(12)
    .align_y(Center);

    let grid = canvas(Grid {
        game,
        insert: solve.mode == Mode::Insert,
    })
    .width(FillPortion(3))
    .height(Fill);
    let clues = row![
        clue_list(game, Direction::Across),
        clue_list(game, Direction::Down)
    ]
    .spacing(16)
    .width(FillPortion(2));

    column![
        header,
        row![grid, clues].spacing(16).height(Fill),
        clue_bar(game),
        mode_line(app, solve),
    ]
    .spacing(10)
    .padding(16)
    .into()
}

fn clue_list(game: &Game, direction: Direction) -> Element<'_, Message> {
    let active = game.current_entry();
    let crossing = game.crossing_entry();
    let rows = game
        .puzzle()
        .entries()
        .iter()
        .enumerate()
        .filter(|(_, e)| e.direction == direction)
        .map(|(i, entry)| {
            let kind = if i == active {
                ClueKind::Active
            } else if Some(i) == crossing {
                ClueKind::Crossing
            } else if game.entry_is_full(i) {
                ClueKind::Done
            } else {
                ClueKind::Plain
            };
            let clue = if entry.clue.is_empty() {
                "(no clue)"
            } else {
                entry.clue.as_str()
            };
            button(
                row![
                    text(entry.number).font(BOLD).width(30),
                    text(clue).width(Fill)
                ]
                .spacing(6),
            )
            .width(Fill)
            .padding([4, 8])
            .style(style::clue(kind))
            .on_press(Message::Clue(i))
            .into()
        });
    let focused = game.puzzle().entry(active).direction == direction;
    let title = text(direction.name().to_uppercase())
        .size(14)
        .font(BOLD)
        .color(if focused { ACCENT } else { MUTED });
    let id = match direction {
        Direction::Across => ACROSS_LIST,
        Direction::Down => DOWN_LIST,
    };
    column![
        title,
        scrollable(Column::with_children(rows).spacing(1))
            .id(id)
            .height(Fill)
    ]
    .spacing(6)
    .width(Fill)
    .into()
}

fn clue_bar(game: &Game) -> Element<'_, Message> {
    let entry = game.puzzle().entry(game.current_entry());
    let clue = if entry.clue.is_empty() {
        "(no clue)"
    } else {
        entry.clue.as_str()
    };
    let mut lines = column![
        row![
            text(format!("{}{}", entry.number, entry.direction.letter()))
                .size(20)
                .font(BOLD)
                .color(ACCENT),
            text(clue).size(20),
        ]
        .spacing(12)
    ]
    .spacing(4);
    if let Some(crossing) = game.crossing_entry() {
        let other = game.puzzle().entry(crossing);
        lines = lines.push(
            text(format!(
                "{}{}  {}",
                other.number,
                other.direction.letter(),
                other.clue
            ))
            .size(14)
            .color(MUTED),
        );
    }
    container(lines)
        .padding([10, 14])
        .width(Fill)
        .style(style::clue_bar)
        .into()
}

fn mode_line<'a>(app: &'a App, solve: &'a Solve) -> Element<'a, Message> {
    match solve.mode {
        Mode::Command => row![text(format!(":{}▏", solve.input)).font(Font::MONOSPACE)].into(),
        Mode::Rebus => row![
            badge("REBUS", REVEALED),
            text(format!("{}▏", solve.input)).font(Font::MONOSPACE),
            text("Enter fills the square · Esc cancels").color(MUTED),
        ]
        .spacing(10)
        .align_y(Center)
        .into(),
        Mode::Normal | Mode::Insert => {
            let (label, colour) = if solve.mode == Mode::Insert {
                ("INSERT", CORRECT)
            } else {
                ("NORMAL", ACCENT)
            };
            let (filled, open) = solve.game.fill_counts();
            row![
                badge(label, colour),
                status_line(app),
                space::horizontal(),
                text(solve.showcmd()).font(Font::MONOSPACE),
                text(format!("{filled}/{open}")).color(MUTED),
                button(text("? Help"))
                    .style(style::link)
                    .on_press(Message::Help),
            ]
            .spacing(12)
            .align_y(Center)
            .into()
        }
    }
}

// ----- help ------------------------------------------------------------------

fn help_section(section: &Section) -> Element<'static, Message> {
    let rows = section.rows.iter().map(|&(keys, what)| {
        row![
            text(keys).font(Font::MONOSPACE).size(14).width(180),
            text(what).size(14)
        ]
        .into()
    });
    column![
        text(section.title).font(BOLD).color(ACCENT),
        Column::with_children(rows).spacing(3)
    ]
    .spacing(6)
    .into()
}

fn help_overlay(base: Element<'_, Message>) -> Element<'_, Message> {
    let columns = help::COLUMNS.iter().map(|sections| {
        Column::with_children(sections.iter().map(help_section))
            .spacing(16)
            .width(Fill)
            .into()
    });
    let card = container(
        column![
            row![
                text("Keys").size(24).font(BOLD),
                space::horizontal(),
                text("Esc or a click closes").color(MUTED),
            ]
            .align_y(Center),
            scrollable(row(columns).spacing(32)),
        ]
        .spacing(16),
    )
    .padding(24)
    .max_width(920)
    .style(style::card);
    let backdrop =
        mouse_area(center(card).padding(24).style(style::backdrop)).on_press(Message::CloseHelp);
    stack![base, opaque(backdrop)].into()
}
