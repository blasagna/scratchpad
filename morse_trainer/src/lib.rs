//! A terminal Morse-code practice trainer.
//!
//! Characters are keyed with two distinct inputs — a dot key and a dash key —
//! and a pause longer than the configured timeout ends the character. This
//! follows the `~/code/morse` project's model of avoiding press-and-hold timing.
//! Two practice modes are offered: free text entry, and a prompted mode that
//! asks the learner to reproduce progressive lessons or short English phrases.

pub mod app;
pub mod lessons;
pub mod morse;
pub mod ui;

pub use app::{App, Mode, Screen};
pub use lessons::Track;
pub use morse::{InputEvent, Symbol};
