//! Key presses in a form that no frontend owns.
//!
//! The TUI converts crossterm events into a [`Key`] and the GUI converts iced
//! events, so the vim-style key handling in `app` is the same code for both.

/// The key itself, after the keyboard layout and Shift are applied: `A` is
/// `Char('A')`, and Shift-Tab is `BackTab`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum KeyCode {
    Char(char),
    Enter,
    Esc,
    Backspace,
    Delete,
    Tab,
    BackTab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
}

/// The modifiers the key handling cares about. Shift is already part of the
/// [`KeyCode`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Modifiers {
    pub ctrl: bool,
    pub alt: bool,
}

/// One key press.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Key {
    pub code: KeyCode,
    pub modifiers: Modifiers,
}

impl Key {
    /// A key with no modifiers.
    pub const fn new(code: KeyCode) -> Key {
        Key {
            code,
            modifiers: Modifiers {
                ctrl: false,
                alt: false,
            },
        }
    }

    /// `Ctrl` and a character, such as `Ctrl-c`.
    pub const fn ctrl(c: char) -> Key {
        Key {
            code: KeyCode::Char(c),
            modifiers: Modifiers {
                ctrl: true,
                alt: false,
            },
        }
    }

    /// `Alt` and a key.
    pub const fn alt(code: KeyCode) -> Key {
        Key {
            code,
            modifiers: Modifiers {
                ctrl: false,
                alt: true,
            },
        }
    }

    pub fn is_ctrl(self) -> bool {
        self.modifiers.ctrl
    }

    /// True for Ctrl or Alt chords, which never type a letter.
    pub fn is_chorded(self) -> bool {
        self.modifiers.ctrl || self.modifiers.alt
    }

    /// The key without its Alt bit, when Alt is held without Ctrl. Terminals
    /// send Alt+x as Esc then x, so this is also what a quick Esc and x look
    /// like.
    pub fn esc_prefixed(self) -> Option<Key> {
        (self.modifiers.alt && !self.modifiers.ctrl).then_some(Key {
            code: self.code,
            modifiers: Modifiers {
                alt: false,
                ..self.modifiers
            },
        })
    }
}

impl From<KeyCode> for Key {
    fn from(code: KeyCode) -> Key {
        Key::new(code)
    }
}

/// Keys for each character of `text`, with `⎋` for Esc, `⏎` for Enter, `⌫`
/// for Backspace and `⇥` for Tab. Tests in every crate type keys this way.
pub fn typed(text: &str) -> Vec<Key> {
    text.chars()
        .map(|c| {
            Key::new(match c {
                '⎋' => KeyCode::Esc,
                '⏎' => KeyCode::Enter,
                '⌫' => KeyCode::Backspace,
                '⇥' => KeyCode::Tab,
                c => KeyCode::Char(c),
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alt_strips_to_the_plain_key() {
        let key = Key::alt(KeyCode::Char('q'));
        assert_eq!(key.esc_prefixed(), Some(Key::new(KeyCode::Char('q'))));
        assert_eq!(Key::ctrl('c').esc_prefixed(), None);
        assert!(Key::ctrl('c').is_chorded());
        assert!(!Key::new(KeyCode::Char('c')).is_chorded());
    }

    #[test]
    fn typed_maps_symbols_to_named_keys() {
        assert_eq!(
            typed("a⎋⏎"),
            [
                Key::new(KeyCode::Char('a')),
                Key::new(KeyCode::Esc),
                Key::new(KeyCode::Enter)
            ]
        );
    }
}
