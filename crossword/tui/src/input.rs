//! Converts crossterm key events into the core's frontend-neutral keys.

use crossterm::event::{KeyCode as TermCode, KeyEvent, KeyModifiers};
use crossword_core::keys::{Key, KeyCode, Modifiers};

/// The core key for a crossterm key event, or `None` for keys the app does
/// not use, such as function keys.
pub fn key(event: KeyEvent) -> Option<Key> {
    let code = match event.code {
        TermCode::Char(c) => KeyCode::Char(c),
        TermCode::Enter => KeyCode::Enter,
        TermCode::Esc => KeyCode::Esc,
        TermCode::Backspace => KeyCode::Backspace,
        TermCode::Delete => KeyCode::Delete,
        TermCode::Tab => KeyCode::Tab,
        TermCode::BackTab => KeyCode::BackTab,
        TermCode::Left => KeyCode::Left,
        TermCode::Right => KeyCode::Right,
        TermCode::Up => KeyCode::Up,
        TermCode::Down => KeyCode::Down,
        TermCode::Home => KeyCode::Home,
        TermCode::End => KeyCode::End,
        TermCode::PageUp => KeyCode::PageUp,
        TermCode::PageDown => KeyCode::PageDown,
        _ => return None,
    };
    Some(Key {
        code,
        modifiers: Modifiers {
            ctrl: event.modifiers.contains(KeyModifiers::CONTROL),
            alt: event.modifiers.contains(KeyModifiers::ALT),
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys_and_modifiers_convert() {
        let ctrl_c = KeyEvent::new(TermCode::Char('c'), KeyModifiers::CONTROL);
        assert_eq!(key(ctrl_c), Some(Key::ctrl('c')));
        let alt_q = KeyEvent::new(TermCode::Char('q'), KeyModifiers::ALT);
        assert_eq!(key(alt_q), Some(Key::alt(KeyCode::Char('q'))));
        // Shift is already in the character.
        let upper = KeyEvent::new(TermCode::Char('G'), KeyModifiers::SHIFT);
        assert_eq!(key(upper), Some(Key::new(KeyCode::Char('G'))));
        let back_tab = KeyEvent::new(TermCode::BackTab, KeyModifiers::SHIFT);
        assert_eq!(key(back_tab), Some(Key::new(KeyCode::BackTab)));
        assert_eq!(key(KeyEvent::new(TermCode::F(1), KeyModifiers::NONE)), None);
    }
}
