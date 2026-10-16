//! The shared core of the crossword app: everything but drawing and input.
//!
//! Puzzles come in three sizes after the NYT's Mini, Midi and Crossword, from
//! free sources, or from the NYT itself with the player's own subscription
//! cookie. This crate holds the puzzle model, the solving rules, the sources,
//! the files on disk, and the screen and vim-mode state machine in [`app`].
//! The TUI (`crossword_tui`) and the GUI (`crossword_gui`) only convert their
//! input into [`keys::Key`] presses and [`app::App`] calls, and draw its
//! state. Nothing here touches a terminal or a window, so all of it is
//! testable on its own.

pub mod app;
pub mod game;
pub mod help;
pub mod keys;
pub mod puzzle;
pub mod sources;
pub mod store;
