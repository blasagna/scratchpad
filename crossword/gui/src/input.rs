//! Converts iced keyboard events into the core's frontend-neutral keys, so
//! the GUI runs the same vim-style key handling as the TUI.

use crossword_core::keys::{Key, KeyCode, Modifiers};
use iced::keyboard::{self, key::Named};

/// The core key for an iced key press, or `None` for keys the app does not
/// use. `key` is the key as pressed and `modified_key` the key with Shift and
/// the layout applied, so Shift+g gives `G` and Shift+; gives `:`.
pub fn key(
    key: &keyboard::Key,
    modified_key: &keyboard::Key,
    modifiers: keyboard::Modifiers,
) -> Option<Key> {
    let code = match key.as_ref() {
        keyboard::Key::Named(named) => match named {
            Named::Enter => KeyCode::Enter,
            Named::Escape => KeyCode::Esc,
            Named::Backspace => KeyCode::Backspace,
            Named::Delete => KeyCode::Delete,
            Named::Tab if modifiers.shift() => KeyCode::BackTab,
            Named::Tab => KeyCode::Tab,
            Named::ArrowLeft => KeyCode::Left,
            Named::ArrowRight => KeyCode::Right,
            Named::ArrowUp => KeyCode::Up,
            Named::ArrowDown => KeyCode::Down,
            Named::Home => KeyCode::Home,
            Named::End => KeyCode::End,
            Named::PageUp => KeyCode::PageUp,
            Named::PageDown => KeyCode::PageDown,
            Named::Space => KeyCode::Char(' '),
            _ => return None,
        },
        keyboard::Key::Character(plain) => {
            let text = match modified_key.as_ref() {
                keyboard::Key::Character(shifted) => shifted,
                _ => plain,
            };
            KeyCode::Char(text.chars().next()?)
        }
        keyboard::Key::Unidentified => return None,
    };
    Some(Key {
        code,
        modifiers: Modifiers {
            ctrl: modifiers.control(),
            alt: modifiers.alt(),
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use keyboard::Modifiers as M;

    fn char_key(c: &str) -> keyboard::Key {
        keyboard::Key::Character(c.into())
    }

    #[test]
    fn characters_take_shift_from_the_modified_key() {
        assert_eq!(
            key(&char_key("g"), &char_key("G"), M::SHIFT),
            Some(Key::new(KeyCode::Char('G')))
        );
        assert_eq!(
            key(&char_key(";"), &char_key(":"), M::SHIFT),
            Some(Key::new(KeyCode::Char(':')))
        );
    }

    #[test]
    fn chords_and_named_keys_convert() {
        assert_eq!(
            key(&char_key("c"), &char_key("c"), M::CTRL),
            Some(Key::ctrl('c'))
        );
        assert_eq!(
            key(&char_key("q"), &char_key("q"), M::ALT),
            Some(Key::alt(KeyCode::Char('q')))
        );
        let tab = keyboard::Key::Named(Named::Tab);
        assert_eq!(key(&tab, &tab, M::SHIFT), Some(Key::new(KeyCode::BackTab)));
        let esc = keyboard::Key::Named(Named::Escape);
        assert_eq!(key(&esc, &esc, M::empty()), Some(Key::new(KeyCode::Esc)));
        let space = keyboard::Key::Named(Named::Space);
        assert_eq!(
            key(&space, &space, M::empty()),
            Some(Key::new(KeyCode::Char(' ')))
        );
        let f1 = keyboard::Key::Named(Named::F1);
        assert_eq!(key(&f1, &f1, M::empty()), None);
    }
}
