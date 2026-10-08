//! The GUI's colours and widget styles: the NYT's paper look for the grid,
//! and a quiet light theme around it.

use iced::widget::{button, container};
use iced::{Background, Border, Color, Theme};

pub const PAPER: Color = Color::WHITE;
pub const BLOCK: Color = Color::from_rgb8(0x1a, 0x1a, 0x1a);
pub const WORD: Color = Color::from_rgb8(0xa7, 0xd8, 0xff);
pub const CURSOR_NORMAL: Color = Color::from_rgb8(0xff, 0xda, 0x00);
pub const CURSOR_INSERT: Color = Color::from_rgb8(0x9b, 0xe2, 0x9b);
pub const LINES: Color = Color::from_rgb8(0x8c, 0x8c, 0x8c);
pub const NUMBER: Color = Color::from_rgb8(0x33, 0x33, 0x33);
pub const INK: Color = Color::BLACK;
pub const WRONG: Color = Color::from_rgb8(0xd0, 0x02, 0x1b);
pub const REVEALED: Color = Color::from_rgb8(0x8e, 0x24, 0xaa);
pub const CORRECT: Color = Color::from_rgb8(0x2e, 0x7d, 0x32);
pub const ACCENT: Color = Color::from_rgb8(0x28, 0x60, 0xd8);
pub const MUTED: Color = Color::from_rgb8(0x70, 0x70, 0x70);
pub const STARTED: Color = Color::from_rgb8(0xb2, 0x6a, 0x00);
const SURFACE: Color = Color::from_rgb8(0xf3, 0xf4, 0xf6);
const HOVER: Color = Color::from_rgb8(0xe8, 0xec, 0xf2);
const CROSSING: Color = Color::from_rgb8(0xe3, 0xf1, 0xff);

fn rounded(radius: f32) -> Border {
    Border {
        radius: radius.into(),
        ..Border::default()
    }
}

fn hovered(status: button::Status) -> bool {
    matches!(status, button::Status::Hovered | button::Status::Pressed)
}

/// A size on the home screen. The selected one is filled with the accent.
pub fn choice(selected: bool) -> impl Fn(&Theme, button::Status) -> button::Style {
    move |_, status| {
        let (background, text) = match (selected, hovered(status)) {
            (true, _) => (ACCENT, Color::WHITE),
            (false, true) => (HOVER, INK),
            (false, false) => (SURFACE, INK),
        };
        button::Style {
            background: Some(Background::Color(background)),
            text_color: text,
            border: rounded(10.0),
            ..button::Style::default()
        }
    }
}

/// A source tab on the browse screen.
pub fn tab(active: bool) -> impl Fn(&Theme, button::Status) -> button::Style {
    move |_, status| {
        let (background, text) = match (active, hovered(status)) {
            (true, _) => (ACCENT, Color::WHITE),
            (false, true) => (HOVER, INK),
            (false, false) => (Color::TRANSPARENT, INK),
        };
        button::Style {
            background: Some(Background::Color(background)),
            text_color: text,
            border: rounded(16.0),
            ..button::Style::default()
        }
    }
}

/// A row in a list: the selected row takes the word colour of the grid.
pub fn list_item(selected: bool) -> impl Fn(&Theme, button::Status) -> button::Style {
    move |_, status| {
        let background = match (selected, hovered(status)) {
            (true, _) => WORD,
            (false, true) => HOVER,
            (false, false) => Color::TRANSPARENT,
        };
        button::Style {
            background: Some(Background::Color(background)),
            text_color: INK,
            border: rounded(4.0),
            ..button::Style::default()
        }
    }
}

/// How a clue relates to the cursor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClueKind {
    /// The entry under the cursor.
    Active,
    /// The entry that crosses it.
    Crossing,
    /// An entry with every square filled.
    Done,
    Plain,
}

pub fn clue(kind: ClueKind) -> impl Fn(&Theme, button::Status) -> button::Style {
    move |_, status| {
        let background = match kind {
            ClueKind::Active => WORD,
            ClueKind::Crossing => CROSSING,
            _ if hovered(status) => HOVER,
            _ => Color::TRANSPARENT,
        };
        let text = if kind == ClueKind::Done { MUTED } else { INK };
        button::Style {
            background: Some(Background::Color(background)),
            text_color: text,
            border: rounded(4.0),
            ..button::Style::default()
        }
    }
}

/// A plain text button, for "back" links.
pub fn link(_: &Theme, status: button::Status) -> button::Style {
    button::Style {
        background: hovered(status).then_some(Background::Color(HOVER)),
        text_color: ACCENT,
        border: rounded(6.0),
        ..button::Style::default()
    }
}

/// A small filled label, such as the source or the vim mode.
pub fn badge(colour: Color) -> impl Fn(&Theme) -> container::Style {
    move |_| container::Style {
        background: Some(Background::Color(colour)),
        text_color: Some(Color::WHITE),
        border: rounded(4.0),
        ..container::Style::default()
    }
}

/// The bar that shows the current clue under the grid.
pub fn clue_bar(_: &Theme) -> container::Style {
    container::Style {
        background: Some(Background::Color(CROSSING)),
        border: rounded(8.0),
        ..container::Style::default()
    }
}

/// The help card.
pub fn card(_: &Theme) -> container::Style {
    container::Style {
        background: Some(Background::Color(Color::WHITE)),
        border: Border {
            radius: 12.0.into(),
            width: 1.0,
            color: LINES,
        },
        ..container::Style::default()
    }
}

/// The dimmed backdrop behind the help card.
pub fn backdrop(_: &Theme) -> container::Style {
    container::Style {
        background: Some(Background::Color(Color {
            a: 0.45,
            ..Color::BLACK
        })),
        ..container::Style::default()
    }
}
