//! A terminal crossword player with vim-style keys.
//!
//! Puzzles come in three sizes after the NYT's Mini, Midi and Crossword, from
//! free sources, or from the NYT itself with the player's own subscription
//! cookie. The library holds everything but the terminal setup and the event
//! loop, so the solving rules, the key handling and the rendering are all
//! testable without a terminal or the network.

pub mod app;
pub mod game;
pub mod puzzle;
pub mod sources;
pub mod store;
pub mod ui;
