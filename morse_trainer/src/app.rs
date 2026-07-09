//! Terminal-independent application state and transitions.
//!
//! All practice logic lives here as plain methods on [`App`] so it can be unit
//! tested without a terminal. The event loop in `main` translates key presses
//! into these calls and fires [`App::commit_sequence`] when the input pause
//! timeout elapses.

use crate::lessons::{self, Track};
use crate::morse::{self, InputEvent, Symbol};

/// Which screen the user is currently on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Screen {
    /// Top-level choice between the two practice modes.
    Menu,
    /// Choosing a prompt track/lesson before prompted practice.
    LessonPicker,
    /// Active keying, either free text or prompted.
    Practice,
}

/// The active practice mode once on the [`Screen::Practice`] screen.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Key freely; every decoded character is appended to a buffer.
    FreeText,
    /// Reproduce a given prompt one character at a time.
    Prompted,
}

/// The complete UI state.
pub struct App {
    pub screen: Screen,
    pub mode: Mode,
    pub track: Track,
    /// Selected lesson index (used when `track == Track::Lessons`).
    pub lesson: usize,
    /// Position within the track's prompts; advances as prompts are completed.
    pub prompt_index: usize,
    /// Highlighted item on the current menu screen.
    pub selection: usize,
    /// Free-text decoded output.
    pub output: String,
    /// Characters produced so far against the current prompt.
    pub typed: String,
    /// The prompt currently being reproduced.
    pub target: String,
    /// Whether to reveal the Morse for the next expected character.
    pub show_hint: bool,
    /// Set when the user asks to quit.
    pub should_quit: bool,
    /// In-progress dot/dash sequence, not yet committed.
    seq: Vec<InputEvent>,
}

impl Default for App {
    fn default() -> Self {
        App {
            screen: Screen::Menu,
            mode: Mode::FreeText,
            track: Track::Phrases,
            lesson: 0,
            prompt_index: 0,
            selection: 0,
            output: String::new(),
            typed: String::new(),
            target: String::new(),
            show_hint: false,
            should_quit: false,
            seq: Vec::new(),
        }
    }
}

impl App {
    pub fn new() -> Self {
        App::default()
    }

    /// True while on the practice screen (where dot/dash keys are meaningful).
    pub fn in_practice(&self) -> bool {
        self.screen == Screen::Practice
    }

    /// The in-progress, not-yet-committed sequence.
    pub fn pending(&self) -> &[InputEvent] {
        &self.seq
    }

    /// True when there is a partial sequence awaiting the commit timeout.
    pub fn has_pending(&self) -> bool {
        !self.seq.is_empty()
    }

    /// Number of selectable items on the current menu screen.
    pub fn menu_len(&self) -> usize {
        match self.screen {
            Screen::Menu => 2,                                   // Free text, Prompted
            Screen::LessonPicker => 1 + lessons::lesson_count(), // Phrases + lessons
            Screen::Practice => 0,
        }
    }

    /// Appends a dot or dash to the in-progress sequence (practice screen only).
    pub fn push(&mut self, event: InputEvent) {
        if self.screen == Screen::Practice {
            self.seq.push(event);
        }
    }

    /// Decodes and applies the in-progress sequence, then clears it. Called by
    /// the event loop once the input pause timeout elapses.
    pub fn commit_sequence(&mut self) {
        let decoded = morse::decode(&self.seq);
        self.seq.clear();
        let Some(symbol) = decoded else {
            return; // unrecognized sequence — nothing to apply
        };
        match self.mode {
            Mode::FreeText => match symbol {
                Symbol::Char(c) => self.output.push(c),
                Symbol::Backspace => {
                    self.output.pop();
                }
            },
            Mode::Prompted => match symbol {
                Symbol::Char(c) => {
                    self.typed.push(c);
                    if self.typed == self.target {
                        self.next_prompt();
                    }
                }
                Symbol::Backspace => {
                    self.typed.pop();
                }
            },
        }
    }

    /// The next character the learner is expected to key, if the prompt is not
    /// yet complete.
    pub fn next_expected_char(&self) -> Option<char> {
        self.target.chars().nth(self.typed.chars().count())
    }

    /// Toggles the Morse hint for the next expected character.
    pub fn toggle_hint(&mut self) {
        self.show_hint = !self.show_hint;
    }

    /// Moves the menu highlight up, wrapping around.
    pub fn menu_up(&mut self) {
        let len = self.menu_len();
        if len > 0 {
            self.selection = (self.selection + len - 1) % len;
        }
    }

    /// Moves the menu highlight down, wrapping around.
    pub fn menu_down(&mut self) {
        let len = self.menu_len();
        if len > 0 {
            self.selection = (self.selection + 1) % len;
        }
    }

    /// Activates the highlighted menu item.
    pub fn menu_select(&mut self) {
        match self.screen {
            Screen::Menu => match self.selection {
                0 => self.start_free_text(),
                _ => {
                    self.screen = Screen::LessonPicker;
                    self.selection = 0;
                }
            },
            Screen::LessonPicker => {
                if self.selection == 0 {
                    self.start_prompted(Track::Phrases, 0);
                } else {
                    self.start_prompted(Track::Lessons, self.selection - 1);
                }
            }
            Screen::Practice => {}
        }
    }

    /// Backs out of the current screen: practice/picker return to the menu; the
    /// menu itself requests quit.
    pub fn back(&mut self) {
        match self.screen {
            Screen::Practice | Screen::LessonPicker => {
                self.screen = Screen::Menu;
                self.selection = 0;
                self.seq.clear();
            }
            Screen::Menu => self.should_quit = true,
        }
    }

    fn start_free_text(&mut self) {
        self.screen = Screen::Practice;
        self.mode = Mode::FreeText;
        self.output.clear();
        self.seq.clear();
    }

    fn start_prompted(&mut self, track: Track, lesson: usize) {
        self.screen = Screen::Practice;
        self.mode = Mode::Prompted;
        self.track = track;
        self.lesson = lesson;
        self.prompt_index = 0;
        self.seq.clear();
        self.load_prompt();
    }

    fn next_prompt(&mut self) {
        self.prompt_index += 1;
        self.load_prompt();
    }

    fn load_prompt(&mut self) {
        self.target = lessons::prompt(self.track, self.lesson, self.prompt_index).to_string();
        self.typed.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::morse::InputEvent::{Dash, Dot};

    fn free_text_app() -> App {
        let mut app = App::new();
        app.menu_select(); // selection 0 = Free text
        assert_eq!(app.mode, Mode::FreeText);
        app
    }

    #[test]
    fn free_text_appends_decoded_chars() {
        let mut app = free_text_app();
        app.push(Dot);
        app.push(Dot);
        app.commit_sequence(); // ".." -> i
        app.push(Dot);
        app.push(Dash);
        app.commit_sequence(); // ".-" -> a
        assert_eq!(app.output, "ia");
    }

    #[test]
    fn free_text_space_and_backspace() {
        let mut app = free_text_app();
        app.push(Dot);
        app.commit_sequence(); // e
        app.push(Dot);
        app.push(Dot);
        app.push(Dash);
        app.push(Dash);
        app.commit_sequence(); // "..--" -> space
        assert_eq!(app.output, "e ");
        app.push(Dash);
        app.push(Dash);
        app.push(Dash);
        app.push(Dash);
        app.commit_sequence(); // "----" -> backspace removes the space
        assert_eq!(app.output, "e");
    }

    #[test]
    fn invalid_sequence_is_ignored() {
        let mut app = free_text_app();
        app.push(Dot);
        app.push(Dash);
        app.push(Dot);
        app.push(Dash); // ".-.-" is unused
        app.commit_sequence();
        assert_eq!(app.output, "");
        assert!(!app.has_pending());
    }

    #[test]
    fn prompted_advances_on_completion() {
        let mut app = App::new();
        // Menu -> Prompted -> LessonPicker; pick lesson 1 (E T).
        app.menu_down();
        app.menu_select();
        assert_eq!(app.screen, Screen::LessonPicker);
        app.selection = 1; // first lesson
        app.menu_select();
        assert_eq!(app.mode, Mode::Prompted);
        assert_eq!(app.target, "e");
        assert_eq!(app.next_expected_char(), Some('e'));

        app.push(Dot);
        app.commit_sequence(); // "e" completes the prompt
        assert_eq!(app.prompt_index, 1);
        assert_eq!(app.target, "t");
        assert_eq!(app.typed, "");
    }

    #[test]
    fn prompted_wrong_char_then_backspace() {
        let mut app = App::new();
        app.menu_down();
        app.menu_select();
        app.selection = 1;
        app.menu_select(); // lesson "E T", target "e"

        app.push(Dash);
        app.commit_sequence(); // "t" — wrong, no advance
        assert_eq!(app.typed, "t");
        assert_eq!(app.prompt_index, 0);

        app.push(Dash);
        app.push(Dash);
        app.push(Dash);
        app.push(Dash);
        app.commit_sequence(); // backspace removes the wrong char
        assert_eq!(app.typed, "");

        app.push(Dot);
        app.commit_sequence(); // correct now
        assert_eq!(app.prompt_index, 1);
    }

    #[test]
    fn push_ignored_outside_practice() {
        let mut app = App::new();
        app.push(Dot);
        assert!(!app.has_pending());
    }

    #[test]
    fn menu_navigation_wraps() {
        let mut app = App::new();
        assert_eq!(app.selection, 0);
        app.menu_up();
        assert_eq!(app.selection, 1); // wrapped to last
        app.menu_down();
        assert_eq!(app.selection, 0);
    }

    #[test]
    fn back_from_practice_returns_to_menu() {
        let mut app = free_text_app();
        app.back();
        assert_eq!(app.screen, Screen::Menu);
        app.back();
        assert!(app.should_quit);
    }
}
