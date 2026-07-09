//! Core Morse domain: the dot/dash input model and the decode table.
//!
//! This is a Rust port of the tables and decode logic in the `~/code/morse`
//! project (`legacy-go/morse.go`). Following that project's philosophy, a
//! character is keyed with two distinct inputs — a dot and a dash — rather than
//! press-and-hold durations, which removes most of Morse's timing
//! specifications. Two extensions to standard Morse are preserved:
//!
//! - **space** = dot-dot-dash-dash (`..--`)
//! - **backspace** = dash-dash-dash-dash (`----`)
//!
//! The table is ordered by Morse's dichotomic (binary-tree) structure —
//! shortest and most common first (E, T → I, A, N, M → …) — which doubles as a
//! natural learning progression and drives the lessons in [`crate::lessons`].

/// A single keyed input: a dot, a dash, or anything else.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputEvent {
    Dot,
    Dash,
    Invalid,
}

/// The decoded result of a completed dot/dash sequence.
///
/// A space is represented as `Char(' ')`; backspace is its own variant because
/// it edits the output rather than appending to it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Symbol {
    Char(char),
    Backspace,
}

/// Longest sequence the decoder recognizes (the digits); longer input is
/// truncated before lookup, matching the legacy decoder.
pub const MAX_SEQ_LEN: usize = 5;

use InputEvent::{Dash, Dot};

/// The full Morse table in dichotomic order.
///
/// Ordering is significant: it reproduces `legacy-go/morse.go` exactly (E, T /
/// I, A, N, M / S, U, R, W, D, K, G, O / …) so that both decoding and the
/// letter-introduction order used by the lessons stay faithful to the source.
/// The two unused four-element slots (`.-.-` and `---.`) are intentionally
/// absent, as in the original.
static TABLE: &[(Symbol, &[InputEvent])] = &[
    // length 1
    (Symbol::Char('e'), &[Dot]),
    (Symbol::Char('t'), &[Dash]),
    // length 2
    (Symbol::Char('i'), &[Dot, Dot]),
    (Symbol::Char('a'), &[Dot, Dash]),
    (Symbol::Char('n'), &[Dash, Dot]),
    (Symbol::Char('m'), &[Dash, Dash]),
    // length 3
    (Symbol::Char('s'), &[Dot, Dot, Dot]),
    (Symbol::Char('u'), &[Dot, Dot, Dash]),
    (Symbol::Char('r'), &[Dot, Dash, Dot]),
    (Symbol::Char('w'), &[Dot, Dash, Dash]),
    (Symbol::Char('d'), &[Dash, Dot, Dot]),
    (Symbol::Char('k'), &[Dash, Dot, Dash]),
    (Symbol::Char('g'), &[Dash, Dash, Dot]),
    (Symbol::Char('o'), &[Dash, Dash, Dash]),
    // length 4
    (Symbol::Char('h'), &[Dot, Dot, Dot, Dot]),
    (Symbol::Char('v'), &[Dot, Dot, Dot, Dash]),
    (Symbol::Char('f'), &[Dot, Dot, Dash, Dot]),
    (Symbol::Char(' '), &[Dot, Dot, Dash, Dash]), // space extension
    (Symbol::Char('l'), &[Dot, Dash, Dot, Dot]),
    (Symbol::Char('p'), &[Dot, Dash, Dash, Dot]),
    (Symbol::Char('j'), &[Dot, Dash, Dash, Dash]),
    (Symbol::Char('b'), &[Dash, Dot, Dot, Dot]),
    (Symbol::Char('x'), &[Dash, Dot, Dot, Dash]),
    (Symbol::Char('c'), &[Dash, Dot, Dash, Dot]),
    (Symbol::Char('y'), &[Dash, Dot, Dash, Dash]),
    (Symbol::Char('z'), &[Dash, Dash, Dot, Dot]),
    (Symbol::Char('q'), &[Dash, Dash, Dot, Dash]),
    (Symbol::Backspace, &[Dash, Dash, Dash, Dash]), // backspace extension
    // length 5
    (Symbol::Char('0'), &[Dash, Dash, Dash, Dash, Dash]),
    (Symbol::Char('1'), &[Dot, Dash, Dash, Dash, Dash]),
    (Symbol::Char('2'), &[Dot, Dot, Dash, Dash, Dash]),
    (Symbol::Char('3'), &[Dot, Dot, Dot, Dash, Dash]),
    (Symbol::Char('4'), &[Dot, Dot, Dot, Dot, Dash]),
    (Symbol::Char('5'), &[Dot, Dot, Dot, Dot, Dot]),
    (Symbol::Char('6'), &[Dash, Dot, Dot, Dot, Dot]),
    (Symbol::Char('7'), &[Dash, Dash, Dot, Dot, Dot]),
    (Symbol::Char('8'), &[Dash, Dash, Dash, Dot, Dot]),
    (Symbol::Char('9'), &[Dash, Dash, Dash, Dash, Dot]),
];

/// Decodes a completed dot/dash sequence into a [`Symbol`].
///
/// Input longer than [`MAX_SEQ_LEN`] is truncated before lookup. Returns `None`
/// for the empty sequence or any sequence that has no table entry (including one
/// containing [`InputEvent::Invalid`]).
pub fn decode(seq: &[InputEvent]) -> Option<Symbol> {
    let seq = if seq.len() > MAX_SEQ_LEN {
        &seq[..MAX_SEQ_LEN]
    } else {
        seq
    };
    if seq.is_empty() {
        return None;
    }
    TABLE
        .iter()
        .find(|(_, candidate)| *candidate == seq)
        .map(|(symbol, _)| *symbol)
}

/// Returns the dot/dash sequence that produces `symbol`, if any. This is the
/// general reverse lookup, used for the space (`..--`) and backspace (`----`)
/// extension reminders as well as [`encode`].
pub fn encode_symbol(symbol: Symbol) -> Option<&'static [InputEvent]> {
    TABLE
        .iter()
        .find(|(candidate, _)| *candidate == symbol)
        .map(|(_, seq)| *seq)
}

/// Returns the dot/dash sequence that decodes to `ch`, if any.
///
/// Used by prompted mode to show the Morse for an expected character. Lookup is
/// case-insensitive for letters; space maps to the `..--` extension.
pub fn encode(ch: char) -> Option<&'static [InputEvent]> {
    encode_symbol(Symbol::Char(ch.to_ascii_lowercase()))
}

/// A human-readable legend of the full table in dichotomic order: each entry
/// pairs a character label with its dot/dash rendering. The space and backspace
/// extensions are labeled by name rather than by an invisible glyph.
///
/// Used by the TUI to show a toggleable Morse reference chart during practice.
pub fn legend_entries() -> Vec<(String, String)> {
    TABLE
        .iter()
        .map(|(symbol, seq)| {
            let label = match symbol {
                Symbol::Char(' ') => "space".to_string(),
                Symbol::Char(c) => c.to_string(),
                Symbol::Backspace => "bksp".to_string(),
            };
            (label, render_sequence(seq))
        })
        .collect()
}

/// Renders a sequence as dots and dashes, e.g. `[Dot, Dash]` → `".-"`.
pub fn render_sequence(seq: &[InputEvent]) -> String {
    seq.iter()
        .map(|e| match e {
            InputEvent::Dot => '.',
            InputEvent::Dash => '-',
            InputEvent::Invalid => '?',
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_table_entry_round_trips() {
        for (symbol, seq) in TABLE {
            assert_eq!(decode(seq), Some(*symbol), "decode of {seq:?}");
            if let Symbol::Char(ch) = symbol {
                // Backspace has no character, so only chars round-trip through encode.
                assert_eq!(encode(*ch), Some(*seq), "encode of {ch:?}");
            }
        }
    }

    #[test]
    fn space_and_backspace_extensions() {
        assert_eq!(decode(&[Dot, Dot, Dash, Dash]), Some(Symbol::Char(' ')));
        assert_eq!(decode(&[Dash, Dash, Dash, Dash]), Some(Symbol::Backspace));
    }

    #[test]
    fn empty_sequence_is_none() {
        assert_eq!(decode(&[]), None);
    }

    #[test]
    fn invalid_input_is_none() {
        assert_eq!(decode(&[Dot, InputEvent::Invalid]), None);
    }

    #[test]
    fn unused_four_element_slots_are_none() {
        assert_eq!(decode(&[Dot, Dash, Dot, Dash]), None);
        assert_eq!(decode(&[Dash, Dash, Dash, Dot]), None);
    }

    #[test]
    fn long_input_is_truncated() {
        // "....." decodes to 5; a trailing sixth element is dropped before lookup.
        assert_eq!(
            decode(&[Dot, Dot, Dot, Dot, Dot, Dash]),
            Some(Symbol::Char('5'))
        );
    }

    #[test]
    fn encode_is_case_insensitive() {
        assert_eq!(encode('E'), encode('e'));
        assert_eq!(encode('E'), Some([Dot].as_slice()));
    }

    #[test]
    fn render_sequence_maps_dots_and_dashes() {
        assert_eq!(render_sequence(&[Dot, Dash, Dot]), ".-.");
    }
}
