//! The terminal frontend: ratatui drawing and crossterm input over the shared
//! `crossword_core`. All game, source and screen logic lives in the core; this
//! crate converts key events into core [`Key`](crossword_core::keys::Key)s and
//! draws the core's state.

pub mod input;
pub mod ui;
