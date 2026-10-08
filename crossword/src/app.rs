//! The application state machine: three screens, and the vim-style modes of
//! the solve screen.
//!
//! The event loop in `main` passes key presses to [`App::handle_key`] and
//! fetch results to [`App::on_response`]. It sends whatever
//! [`App::take_requests`] returns to the fetch thread. Nothing here touches
//! the terminal or the network, so tests drive it with plain key events.

use std::time::{Duration, Instant};

use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};

use crate::game::{Game, Scope};
use crate::puzzle::{Direction, Puzzle};
use crate::sources::{PuzzleRef, Request, Response, Size, SourceId};
use crate::store::{Status, Store};

/// How far `Ctrl-d` and `Ctrl-u` move in a listing.
const PAGE: usize = 10;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Screen {
    /// Choose a size.
    Home,
    /// Choose a puzzle from a source's listing.
    Browse,
    /// Solve a puzzle.
    Solve,
}

/// The modes of the solve screen, after vim's.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Keys move the cursor and run commands.
    Normal,
    /// Keys type letters into the grid.
    Insert,
    /// A `:` command is being typed.
    Command,
    /// Several letters for one rebus square are being typed.
    Rebus,
}

/// A normal-mode key that waits for a second key.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Pending {
    /// `g`, for `gg`.
    G,
    /// `r`, for `r{letter}`.
    Replace,
    /// `d`, for `dd` and `dw`.
    Delete,
    /// `c`, for `cc` and `cw`.
    Change,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tone {
    Info,
    Success,
    Error,
}

/// A line for the status bar.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Message {
    pub text: String,
    pub tone: Tone,
}

impl Message {
    fn info(text: impl Into<String>) -> Message {
        Message {
            text: text.into(),
            tone: Tone::Info,
        }
    }

    fn success(text: impl Into<String>) -> Message {
        Message {
            text: text.into(),
            tone: Tone::Success,
        }
    }

    fn error(text: impl Into<String>) -> Message {
        Message {
            text: text.into(),
            tone: Tone::Error,
        }
    }
}

/// One source's listing, as it loads.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Listing {
    Loading,
    Ready(Vec<Item>),
    Failed(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Item {
    pub puzzle: PuzzleRef,
    pub status: Status,
}

/// One source on the browse screen.
#[derive(Debug, Clone)]
pub struct Tab {
    pub source: SourceId,
    pub listing: Listing,
    pub selected: usize,
}

impl Tab {
    pub fn items(&self) -> &[Item] {
        match &self.listing {
            Listing::Ready(items) => items,
            _ => &[],
        }
    }

    fn move_selection(&mut self, delta: isize) {
        let last = self.items().len().saturating_sub(1);
        self.selected = self.selected.saturating_add_signed(delta).min(last);
    }
}

/// The browse screen: one tab per source of a size.
#[derive(Debug, Clone)]
pub struct Browse {
    pub size: Size,
    pub tabs: Vec<Tab>,
    pub active: usize,
}

impl Browse {
    pub fn tab(&self) -> &Tab {
        &self.tabs[self.active]
    }

    fn tab_mut(&mut self) -> &mut Tab {
        &mut self.tabs[self.active]
    }

    pub fn selected_item(&self) -> Option<&Item> {
        let tab = self.tab();
        tab.items().get(tab.selected)
    }
}

/// The solve screen.
#[derive(Debug, Clone)]
pub struct Solve {
    pub game: Game,
    pub puzzle: PuzzleRef,
    pub mode: Mode,
    /// The command line or rebus text being typed.
    pub input: String,
    pending: Option<Pending>,
    count: Option<usize>,
}

impl Solve {
    fn new(game: Game, puzzle: PuzzleRef) -> Solve {
        Solve {
            game,
            puzzle,
            mode: Mode::Normal,
            input: String::new(),
            pending: None,
            count: None,
        }
    }

    /// The keys typed so far of an unfinished command, as vim's `showcmd`
    /// shows them: `3`, `d` or `g`.
    pub fn showcmd(&self) -> String {
        let mut keys = self.count.map(|c| c.to_string()).unwrap_or_default();
        if let Some(pending) = self.pending {
            keys.push(match pending {
                Pending::G => 'g',
                Pending::Replace => 'r',
                Pending::Delete => 'd',
                Pending::Change => 'c',
            });
        }
        keys
    }

    fn enter_insert(&mut self, checkpoint: bool) -> Option<Message> {
        if self.game.is_solved() {
            return Some(solved_hint());
        }
        if checkpoint {
            self.game.checkpoint();
        }
        self.mode = Mode::Insert;
        None
    }
}

/// What a solve-screen key asks the app to do.
#[derive(Debug, Default)]
struct Outcome {
    message: Option<Message>,
    /// Letters or marks may have changed.
    changed: bool,
    save: bool,
    leave: bool,
    quit: bool,
    help: bool,
}

/// The whole UI state.
pub struct App {
    pub screen: Screen,
    pub home_selected: usize,
    pub browse: Option<Browse>,
    pub solve: Option<Solve>,
    pub message: Option<Message>,
    pub show_help: bool,
    /// How far the help overlay is scrolled.
    pub help_scroll: u16,
    pub should_quit: bool,
    /// The puzzle being downloaded, while a download is in flight.
    pub loading: Option<PuzzleRef>,
    store: Option<Store>,
    requests: Vec<Request>,
    /// Letters typed in insert mode since the last save.
    unsaved: bool,
    last_save: Option<Instant>,
}

/// How often letters typed in insert mode are saved.
const AUTOSAVE: Duration = Duration::from_secs(10);

impl App {
    /// `store` holds saved progress; `None` keeps everything in memory.
    pub fn new(store: Option<Store>) -> App {
        App {
            screen: Screen::Home,
            home_selected: 0,
            browse: None,
            solve: None,
            message: None,
            show_help: false,
            help_scroll: 0,
            should_quit: false,
            loading: None,
            store,
            requests: Vec::new(),
            unsaved: false,
            last_save: None,
        }
    }

    /// Called on every pass of the event loop. Saves letters typed in insert
    /// mode now and then, so a closed terminal loses at most a few seconds.
    pub fn tick(&mut self, now: Instant) {
        let due = self
            .last_save
            .is_none_or(|t| now.saturating_duration_since(t) >= AUTOSAVE);
        if self.unsaved && due {
            self.save(now);
        }
    }

    /// Requests for the fetch thread made since the last call.
    pub fn take_requests(&mut self) -> Vec<Request> {
        std::mem::take(&mut self.requests)
    }

    pub fn handle_key(&mut self, key: KeyEvent, now: Instant) {
        if self.show_help {
            // j and k scroll the help; any other key closes it.
            match key.code {
                KeyCode::Char('j') | KeyCode::Down => {
                    self.help_scroll = (self.help_scroll + 1).min(crate::ui::help_max_scroll())
                }
                KeyCode::Char('k') | KeyCode::Up => {
                    self.help_scroll = self.help_scroll.saturating_sub(1)
                }
                _ => self.show_help = false,
            }
            return;
        }
        match self.screen {
            Screen::Home => self.home_key(key),
            Screen::Browse => self.browse_key(key),
            Screen::Solve => self.solve_key(key, now),
        }
    }

    pub fn on_response(&mut self, response: Response, now: Instant) {
        match response {
            Response::Listed(source, result) => {
                let store = self.store.as_ref();
                let Some(tab) = self
                    .browse
                    .as_mut()
                    .and_then(|b| b.tabs.iter_mut().find(|t| t.source == source))
                else {
                    return;
                };
                tab.listing = match result {
                    Ok(refs) => Listing::Ready(
                        refs.into_iter()
                            .map(|puzzle| Item {
                                status: store.map_or(Status::New, |s| s.status(&puzzle)),
                                puzzle,
                            })
                            .collect(),
                    ),
                    Err(e) => Listing::Failed(e.to_string()),
                };
                tab.move_selection(0);
            }
            Response::Fetched(puzzle, result) => {
                if self.loading.as_ref() != Some(&puzzle) {
                    return; // the player has moved on
                }
                self.loading = None;
                let built = result
                    .map_err(|e| e.to_string())
                    .and_then(|data| Puzzle::new(data).map_err(|e| e.to_string()));
                match built {
                    Ok(built) => self.start_solving(puzzle, built, now),
                    Err(e) => self.message = Some(Message::error(e)),
                }
            }
        }
    }

    /// Goes to the browse screen for `size`, keeping an earlier listing of
    /// the same size.
    pub fn open_size(&mut self, size: Size) {
        self.home_selected = Size::ALL.iter().position(|&s| s == size).unwrap_or(0);
        if self.browse.as_ref().is_none_or(|b| b.size != size) {
            let tabs = size
                .sources()
                .iter()
                .map(|&source| Tab {
                    source,
                    listing: Listing::Loading,
                    selected: 0,
                })
                .collect();
            self.browse = Some(Browse {
                size,
                tabs,
                active: 0,
            });
            self.requests
                .extend(size.sources().iter().map(|&s| Request::List(s)));
        }
        self.message = None;
        self.screen = Screen::Browse;
    }

    /// Pauses the timer while the terminal is out of focus, as the NYT app
    /// does when its tab is hidden, and saves on the way out.
    pub fn set_focus(&mut self, focused: bool, now: Instant) {
        let Some(solve) = self.solve.as_mut() else {
            return;
        };
        if focused {
            solve.game.resume(now);
        } else {
            solve.game.pause(now);
            self.save(now);
        }
    }

    fn open_help(&mut self) {
        self.show_help = true;
        self.help_scroll = 0;
    }

    /// Saves progress, then asks the event loop to exit.
    pub fn quit(&mut self, now: Instant) {
        self.save(now);
        self.should_quit = true;
    }

    fn save(&mut self, now: Instant) {
        self.unsaved = false;
        self.last_save = Some(now);
        let (Some(store), Some(solve)) = (&self.store, &self.solve) else {
            return;
        };
        if let Err(e) = store.save_progress(&solve.puzzle, &solve.game.progress(now)) {
            self.message = Some(Message::error(format!("Could not save progress: {e}")));
        }
    }

    // ----- home ------------------------------------------------------------

    fn home_key(&mut self, key: KeyEvent) {
        self.message = None;
        let last = Size::ALL.len() - 1;
        match key.code {
            KeyCode::Char('c') if ctrl(key) => self.should_quit = true,
            KeyCode::Char('j') | KeyCode::Down => {
                self.home_selected = (self.home_selected + 1).min(last)
            }
            KeyCode::Char('k') | KeyCode::Up => {
                self.home_selected = self.home_selected.saturating_sub(1)
            }
            KeyCode::Char('g') | KeyCode::Home => self.home_selected = 0,
            KeyCode::Char('G') | KeyCode::End => self.home_selected = last,
            KeyCode::Char(c @ '1'..='3') => {
                self.open_size(Size::ALL[c as usize - '1' as usize]);
            }
            KeyCode::Enter | KeyCode::Char('l') | KeyCode::Right => {
                self.open_size(Size::ALL[self.home_selected]);
            }
            KeyCode::Char('q') => self.should_quit = true,
            KeyCode::Char('?') => self.open_help(),
            _ => {}
        }
    }

    // ----- browse ----------------------------------------------------------

    fn browse_key(&mut self, key: KeyEvent) {
        self.message = None;
        let Some(browse) = self.browse.as_mut() else {
            self.screen = Screen::Home;
            return;
        };
        let tabs = browse.tabs.len();
        match key.code {
            KeyCode::Char('c') if ctrl(key) => self.should_quit = true,
            KeyCode::Char('d') if ctrl(key) => browse.tab_mut().move_selection(PAGE as isize),
            KeyCode::Char('u') if ctrl(key) => browse.tab_mut().move_selection(-(PAGE as isize)),
            _ if ctrl(key) => {}
            KeyCode::Char('j') | KeyCode::Down => browse.tab_mut().move_selection(1),
            KeyCode::Char('k') | KeyCode::Up => browse.tab_mut().move_selection(-1),
            KeyCode::PageDown => browse.tab_mut().move_selection(PAGE as isize),
            KeyCode::PageUp => browse.tab_mut().move_selection(-(PAGE as isize)),
            KeyCode::Char('g') | KeyCode::Home => browse.tab_mut().selected = 0,
            KeyCode::Char('G') | KeyCode::End => browse.tab_mut().move_selection(isize::MAX),
            KeyCode::Char('l') | KeyCode::Right | KeyCode::Tab => {
                browse.active = (browse.active + 1) % tabs
            }
            KeyCode::Char('h') | KeyCode::Left | KeyCode::BackTab => {
                browse.active = (browse.active + tabs - 1) % tabs
            }
            KeyCode::Char('r') => {
                let tab = browse.tab_mut();
                tab.listing = Listing::Loading;
                self.requests.push(Request::List(tab.source));
            }
            KeyCode::Enter => self.open_selected(),
            KeyCode::Char('q') | KeyCode::Esc => {
                self.loading = None;
                self.screen = Screen::Home;
            }
            KeyCode::Char('?') => self.open_help(),
            _ => {}
        }
    }

    fn open_selected(&mut self) {
        let Some(item) = self.browse.as_ref().and_then(Browse::selected_item) else {
            return;
        };
        let puzzle = item.puzzle.clone();
        self.message = Some(Message::info(format!(
            "Loading {} {}…",
            puzzle.source.name(),
            puzzle.label()
        )));
        self.loading = Some(puzzle.clone());
        self.requests.push(Request::Fetch(puzzle));
    }

    fn start_solving(&mut self, puzzle: PuzzleRef, built: Puzzle, now: Instant) {
        let progress = self.store.as_ref().and_then(|s| s.load_progress(&puzzle));
        let resumed = progress.is_some();
        let mut game = match progress {
            Some(p) => Game::with_progress(built, p),
            None => Game::new(built),
        };
        game.resume(now);
        self.message = Some(if game.is_solved() {
            solved_hint()
        } else if resumed {
            Message::info("Welcome back. Your letters and time are restored.")
        } else {
            Message::info("Press i to start typing, or ? for help.")
        });
        self.solve = Some(Solve::new(game, puzzle));
        self.screen = Screen::Solve;
    }

    fn leave_puzzle(&mut self, now: Instant) {
        self.save(now);
        if let Some(solve) = self.solve.take() {
            let status = status_of(&solve.game);
            let item = self.browse.as_mut().and_then(|b| {
                b.tabs
                    .iter_mut()
                    .flat_map(|t| match &mut t.listing {
                        Listing::Ready(items) => items.iter_mut(),
                        _ => Default::default(),
                    })
                    .find(|i| i.puzzle == solve.puzzle)
            });
            if let Some(item) = item {
                item.status = status;
            }
        }
        self.screen = if self.browse.is_some() {
            Screen::Browse
        } else {
            Screen::Home
        };
    }

    // ----- solve -----------------------------------------------------------

    fn solve_key(&mut self, key: KeyEvent, now: Instant) {
        let Some(solve) = self.solve.as_mut() else {
            self.screen = Screen::Browse;
            return;
        };
        let was_solved = solve.game.is_solved();
        let was_full = solve.game.is_full();
        let mut out = match solve.mode {
            Mode::Normal => normal_key(solve, key),
            Mode::Insert => insert_key(solve, key),
            Mode::Command => command_key(solve, key, now),
            Mode::Rebus => rebus_key(solve, key),
        };

        let game = &solve.game;
        if !was_solved && game.is_solved() {
            solve.mode = Mode::Normal;
            solve.pending = None;
            solve.count = None;
            out.save = true;
            out.message = Some(Message::success(format!(
                "Solved in {}! :reset starts over, q goes back to the list.",
                format_duration(game.elapsed(now))
            )));
        } else if out.changed && !was_full && game.is_full() && !game.is_solved() {
            out.message = Some(Message::error(
                "Every square is filled, but something is wrong. Try :check puzzle.",
            ));
        }
        if out.changed {
            match solve.mode {
                Mode::Normal => out.save = true,
                _ => self.unsaved = true,
            }
        }

        self.message = out.message;
        if out.help {
            self.open_help();
        }
        if out.quit {
            self.quit(now);
        } else if out.leave {
            self.leave_puzzle(now);
        } else if out.save {
            self.save(now);
        }
    }
}

/// The key without its Alt bit, when Alt is held without Ctrl. Terminals send
/// Alt+x as Esc then x, so this is also what a quick Esc and x look like.
fn esc_prefixed(key: KeyEvent) -> Option<KeyEvent> {
    let alt_only =
        key.modifiers.contains(KeyModifiers::ALT) && !key.modifiers.contains(KeyModifiers::CONTROL);
    alt_only.then(|| KeyEvent::new(key.code, key.modifiers - KeyModifiers::ALT))
}

fn ctrl(key: KeyEvent) -> bool {
    key.modifiers.contains(KeyModifiers::CONTROL)
}

/// True for Ctrl or Alt chords, which never type a letter.
fn chorded(key: KeyEvent) -> bool {
    key.modifiers
        .intersects(KeyModifiers::CONTROL | KeyModifiers::ALT)
}

fn solved_hint() -> Message {
    Message::success("This puzzle is solved. :reset starts over.")
}

fn status_of(game: &Game) -> Status {
    if game.is_solved() {
        Status::Solved
    } else if (0..game.puzzle().len()).any(|c| !game.letter(c).is_empty()) {
        Status::Started
    } else {
        Status::New
    }
}

/// `m:ss`, or `h:mm:ss` from an hour on.
pub fn format_duration(d: Duration) -> String {
    let secs = d.as_secs();
    let (h, m, s) = (secs / 3600, secs / 60 % 60, secs % 60);
    if h > 0 {
        format!("{h}:{m:02}:{s:02}")
    } else {
        format!("{m}:{s:02}")
    }
}

/// Runs one change as an undo step. Returns whether anything changed; a
/// change that changed nothing leaves no undo step behind.
fn edit(game: &mut Game, change: impl FnOnce(&mut Game)) -> bool {
    if game.is_solved() {
        return false;
    }
    game.checkpoint();
    change(game);
    !game.drop_unchanged_checkpoint()
}

fn normal_key(s: &mut Solve, key: KeyEvent) -> Outcome {
    // Alt+x is Esc then x: the Esc cancels a pending operator or count.
    let key = match esc_prefixed(key) {
        Some(plain) => {
            s.pending = None;
            s.count = None;
            plain
        }
        None => key,
    };
    let mut out = Outcome::default();
    let code = key.code;

    // An operator waiting for its second key takes this one.
    if let Some(pending) = s.pending.take() {
        s.count = None;
        let g = &mut s.game;
        match (pending, code) {
            (Pending::G, KeyCode::Char('g')) => g.goto_first(),
            (Pending::Replace, KeyCode::Char(c)) if !chorded(key) && c.is_alphanumeric() => {
                out.changed = edit(g, |g| g.replace_cell(&c.to_string()));
                if !out.changed && !g.editable(g.cursor()) {
                    out.message = Some(locked_hint());
                }
            }
            (Pending::Delete, KeyCode::Char('d')) => out.changed = edit(g, Game::clear_entry),
            (Pending::Delete, KeyCode::Char('w' | 'e' | '$')) => {
                out.changed = edit(g, Game::clear_to_entry_end)
            }
            (Pending::Change, KeyCode::Char('c')) => {
                g.checkpoint();
                g.clear_entry();
                out.message = s.enter_insert(false);
                out.changed = true;
            }
            (Pending::Change, KeyCode::Char('w' | 'e' | '$')) => {
                g.checkpoint();
                g.clear_to_entry_end();
                out.message = s.enter_insert(false);
                out.changed = true;
            }
            // Anything else cancels, as Esc does.
            _ => {}
        }
        return out;
    }

    // A count: `3l`, `12w`. A lone `0` is a motion instead.
    if let KeyCode::Char(d @ '0'..='9') = code
        && !chorded(key)
        && (d != '0' || s.count.is_some())
    {
        let digit = d as usize - '0' as usize;
        s.count = Some((s.count.unwrap_or(0) * 10 + digit).min(999));
        return out;
    }
    let count = s.count.take().unwrap_or(1);
    let g = &mut s.game;
    let repeat = |g: &mut Game, motion: fn(&mut Game)| {
        for _ in 0..count {
            motion(g);
        }
    };

    match code {
        KeyCode::Char('c') if ctrl(key) => {
            out.message = Some(Message::info(
                "Type :q and press Enter to leave the puzzle.",
            ));
        }
        KeyCode::Char('r') if ctrl(key) => {
            let mut any = false;
            for _ in 0..count {
                any |= g.redo();
            }
            out.changed = any;
            if !any {
                out.message = Some(Message::info("Already at the newest change."));
            }
        }
        _ if chorded(key) => {}
        KeyCode::Char('h') | KeyCode::Left => repeat(g, |g| {
            g.step(0, -1);
        }),
        KeyCode::Char('l') | KeyCode::Right => repeat(g, |g| {
            g.step(0, 1);
        }),
        KeyCode::Char('j') | KeyCode::Down => repeat(g, |g| {
            g.step(1, 0);
        }),
        KeyCode::Char('k') | KeyCode::Up => repeat(g, |g| {
            g.step(-1, 0);
        }),
        KeyCode::Char('w') => repeat(g, Game::next_entry_start),
        KeyCode::Char('b') => repeat(g, Game::prev_entry_start),
        KeyCode::Char('e') => repeat(g, Game::entry_end_motion),
        KeyCode::Char('0' | '^') | KeyCode::Home => g.goto_entry_start(),
        KeyCode::Char('$') | KeyCode::End => g.goto_entry_end(),
        KeyCode::Char('g') => s.pending = Some(Pending::G),
        KeyCode::Char('G') => g.goto_last(),
        KeyCode::Tab => repeat(g, |g| g.next_open_entry(true)),
        KeyCode::BackTab => repeat(g, |g| g.next_open_entry(false)),
        KeyCode::Char(' ') | KeyCode::Enter => g.toggle_direction(),
        KeyCode::Char('i') => out.message = s.enter_insert(true),
        KeyCode::Char('a') => {
            g.step_in_entry(true);
            out.message = s.enter_insert(true);
        }
        KeyCode::Char('I') => {
            g.goto_entry_start();
            out.message = s.enter_insert(true);
        }
        KeyCode::Char('A') => {
            g.goto_entry_end();
            out.message = s.enter_insert(true);
        }
        KeyCode::Char('x') | KeyCode::Delete => {
            out.changed = edit(g, Game::clear_cell);
            if !out.changed && !g.letter(g.cursor()).is_empty() {
                out.message = Some(locked_hint());
            }
        }
        KeyCode::Char('D') => out.changed = edit(g, Game::clear_to_entry_end),
        KeyCode::Char('s') => {
            g.checkpoint();
            g.clear_cell();
            out.message = s.enter_insert(false);
            out.changed = true;
        }
        KeyCode::Char('S') => {
            g.checkpoint();
            g.clear_entry();
            out.message = s.enter_insert(false);
            out.changed = true;
        }
        KeyCode::Char('C') => {
            g.checkpoint();
            g.clear_to_entry_end();
            out.message = s.enter_insert(false);
            out.changed = true;
        }
        KeyCode::Char('r') => s.pending = Some(Pending::Replace),
        KeyCode::Char('d') => s.pending = Some(Pending::Delete),
        KeyCode::Char('c') => s.pending = Some(Pending::Change),
        KeyCode::Char('u') => {
            let mut any = false;
            for _ in 0..count {
                any |= g.undo();
            }
            out.changed = any;
            if !any {
                out.message = Some(Message::info("Already at the oldest change."));
            }
        }
        KeyCode::Char('R') => {
            if g.editable(g.cursor()) {
                s.mode = Mode::Rebus;
                s.input.clear();
            } else if g.is_solved() {
                out.message = Some(solved_hint());
            } else {
                out.message = Some(locked_hint());
            }
        }
        KeyCode::Char(':') => {
            s.mode = Mode::Command;
            s.input.clear();
        }
        KeyCode::Char('?') => out.help = true,
        KeyCode::Char('q') => out.leave = true,
        _ => {}
    }
    out
}

fn locked_hint() -> Message {
    Message::info("That square was checked or revealed, so it is locked.")
}

fn insert_key(s: &mut Solve, key: KeyEvent) -> Outcome {
    let mut out = Outcome::default();
    // Terminals send Alt+x as Esc then x, and an Esc typed just before a key
    // can arrive the same way. Like vim, take it as Esc followed by the key.
    if let Some(plain) = esc_prefixed(key) {
        leave_insert(s, &mut out);
        let next = normal_key(s, plain);
        return Outcome {
            save: true,
            changed: out.changed || next.changed,
            ..next
        };
    }
    let g = &mut s.game;
    match key.code {
        KeyCode::Esc => leave_insert(s, &mut out),
        KeyCode::Char('c' | '[') if ctrl(key) => leave_insert(s, &mut out),
        _ if chorded(key) => {}
        KeyCode::Char(' ') => {
            g.clear_and_advance();
            out.changed = true;
        }
        KeyCode::Char(c) if c.is_alphanumeric() => {
            g.type_letters(&c.to_string());
            out.changed = true;
        }
        KeyCode::Backspace => {
            g.backspace();
            out.changed = true;
        }
        KeyCode::Delete => {
            g.clear_cell();
            out.changed = true;
        }
        KeyCode::Left => {
            g.step(0, -1);
        }
        KeyCode::Right => {
            g.step(0, 1);
        }
        KeyCode::Up => {
            g.step(-1, 0);
        }
        KeyCode::Down => {
            g.step(1, 0);
        }
        KeyCode::Tab => g.next_open_entry(true),
        KeyCode::BackTab => g.next_open_entry(false),
        KeyCode::Enter => g.toggle_direction(),
        _ => {}
    }
    out
}

fn leave_insert(s: &mut Solve, out: &mut Outcome) {
    s.mode = Mode::Normal;
    s.game.drop_unchanged_checkpoint();
    out.save = true;
}

fn command_key(s: &mut Solve, key: KeyEvent, now: Instant) -> Outcome {
    match key.code {
        KeyCode::Esc => s.mode = Mode::Normal,
        KeyCode::Char('c' | '[') if ctrl(key) => s.mode = Mode::Normal,
        KeyCode::Enter => {
            s.mode = Mode::Normal;
            let command = std::mem::take(&mut s.input);
            return run_command(s, &command, now);
        }
        KeyCode::Backspace => {
            // As in vim, backspace on an empty command line leaves it.
            if s.input.pop().is_none() {
                s.mode = Mode::Normal;
            }
        }
        KeyCode::Char(c) if !chorded(key) => s.input.push(c),
        _ => {}
    }
    Outcome::default()
}

fn rebus_key(s: &mut Solve, key: KeyEvent) -> Outcome {
    let mut out = Outcome::default();
    match key.code {
        KeyCode::Esc => s.mode = Mode::Normal,
        KeyCode::Char('c' | '[') if ctrl(key) => s.mode = Mode::Normal,
        KeyCode::Enter => {
            s.mode = Mode::Normal;
            let letters = std::mem::take(&mut s.input);
            out.changed = edit(&mut s.game, |g| g.replace_cell(&letters));
        }
        KeyCode::Backspace => {
            s.input.pop();
        }
        KeyCode::Char(c)
            if !chorded(key) && c.is_alphanumeric() && s.input.chars().count() < 12 =>
        {
            s.input.extend(c.to_uppercase());
        }
        _ => {}
    }
    out
}

fn parse_scope(arg: Option<&str>) -> Option<Scope> {
    match arg.unwrap_or("word") {
        "word" | "w" | "entry" | "e" => Some(Scope::Entry),
        "cell" | "c" | "square" | "s" | "letter" | "l" => Some(Scope::Cell),
        "puzzle" | "p" | "all" | "a" | "grid" | "g" => Some(Scope::Puzzle),
        _ => None,
    }
}

/// Reads a clue reference such as `12a`, `12d` or a bare `12`.
fn parse_clue_ref(text: &str) -> Option<(u32, Option<Direction>)> {
    let lower = text.to_ascii_lowercase();
    let (digits, direction) = match lower.strip_suffix(['a', 'd']) {
        Some(digits) => (
            digits,
            Some(if lower.ends_with('a') {
                Direction::Across
            } else {
                Direction::Down
            }),
        ),
        None => (lower.as_str(), None),
    };
    Some((digits.parse().ok()?, direction))
}

fn run_command(s: &mut Solve, command: &str, now: Instant) -> Outcome {
    let mut out = Outcome::default();
    let command = command.trim();
    let (name, arg) = match command.split_once(char::is_whitespace) {
        Some((name, arg)) => (name, Some(arg.trim())),
        None => (command, None),
    };
    let g = &mut s.game;
    match name {
        "" => {}
        "q" | "q!" | "quit" | "wq" | "wq!" | "x" | "x!" => out.leave = true,
        "qa" | "qa!" | "qall" | "quitall" | "wqa" | "xa" => out.quit = true,
        "w" | "write" => {
            out.save = true;
            out.message = Some(Message::info("Progress saved."));
        }
        "check" | "reveal" | "clear" if g.is_solved() => out.message = Some(solved_hint()),
        "check" | "reveal" | "clear" => {
            let Some(scope) = parse_scope(arg) else {
                out.message = Some(Message::error(format!(
                    "Unknown scope {:?}. Use cell, word or puzzle.",
                    arg.unwrap_or_default()
                )));
                return out;
            };
            match name {
                "check" => {
                    let mut report = Default::default();
                    out.changed = edit(g, |g| report = g.check(scope));
                    out.message = Some(match report {
                        crate::game::CheckReport { checked: 0, .. } => {
                            Message::info("There are no letters to check there.")
                        }
                        crate::game::CheckReport { wrong: 0, checked } => {
                            Message::success(format!("All {checked} checked letters are right."))
                        }
                        crate::game::CheckReport { wrong, checked } => Message::error(format!(
                            "{wrong} of {checked} checked letters are wrong."
                        )),
                    });
                }
                "reveal" => {
                    let mut revealed = 0;
                    out.changed = edit(g, |g| revealed = g.reveal(scope));
                    out.message = Some(Message::info(match revealed {
                        0 => "Nothing to reveal there.".to_string(),
                        1 => "Revealed 1 square.".to_string(),
                        n => format!("Revealed {n} squares."),
                    }));
                }
                _ => {
                    out.changed = edit(g, |g| g.clear(scope));
                }
            }
        }
        "reset" => {
            g.reset(now);
            out.changed = true;
            out.message = Some(Message::info("The puzzle and the timer are reset."));
        }
        "help" | "h" => out.help = true,
        _ => match parse_clue_ref(name) {
            Some((number, direction)) => {
                let found = match direction {
                    Some(d) => g.goto_clue(d, number),
                    None => {
                        g.goto_clue(g.direction(), number)
                            || g.goto_clue(g.direction().other(), number)
                    }
                };
                if !found {
                    out.message = Some(Message::error(format!("There is no clue {name}.")));
                }
            }
            None => {
                out.message = Some(Message::error(format!("Not an editor command: {command}")));
            }
        },
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::Mark;
    use crate::puzzle::tests::small;
    use crate::sources::SourceError;

    fn press(app: &mut App, code: KeyCode) {
        app.handle_key(KeyEvent::new(code, KeyModifiers::NONE), Instant::now());
    }

    fn keys(app: &mut App, text: &str) {
        for c in text.chars() {
            let code = match c {
                '⎋' => KeyCode::Esc,
                '⏎' => KeyCode::Enter,
                '⌫' => KeyCode::Backspace,
                '⇥' => KeyCode::Tab,
                c => KeyCode::Char(c),
            };
            press(app, code);
        }
    }

    fn ctrl_key(app: &mut App, c: char) {
        app.handle_key(
            KeyEvent::new(KeyCode::Char(c), KeyModifiers::CONTROL),
            Instant::now(),
        );
    }

    fn reference() -> PuzzleRef {
        PuzzleRef {
            source: SourceId::Universal,
            id: "2026-10-08".into(),
            date: None,
            title: None,
        }
    }

    /// An app on the solve screen of the small 3x3 puzzle, as if the player
    /// had chosen it from the Crossword list.
    fn solving() -> App {
        solving_with(None)
    }

    fn solving_with(store: Option<Store>) -> App {
        let mut app = App::new(store);
        app.open_size(Size::Crossword);
        app.on_response(
            Response::Listed(SourceId::Universal, Ok(vec![reference()])),
            Instant::now(),
        );
        keys(&mut app, "⏎");
        app.on_response(
            Response::Fetched(reference(), Ok(small().data().clone())),
            Instant::now(),
        );
        app.take_requests();
        assert_eq!(app.screen, Screen::Solve);
        app
    }

    fn solve(app: &App) -> &Solve {
        app.solve.as_ref().unwrap()
    }

    fn grid(app: &App) -> String {
        let g = &solve(app).game;
        (0..g.puzzle().len())
            .map(|c| match g.letter(c) {
                _ if !g.puzzle().is_open(c) => "#".to_string(),
                "" => ".".to_string(),
                s => s.to_string(),
            })
            .collect()
    }

    #[test]
    fn home_opens_a_size_and_requests_its_listings() {
        let mut app = App::new(None);
        keys(&mut app, "jj⏎");
        assert_eq!(app.screen, Screen::Browse);
        assert_eq!(app.browse.as_ref().unwrap().size, Size::Crossword);
        assert_eq!(
            app.take_requests(),
            [
                Request::List(SourceId::Universal),
                Request::List(SourceId::Princetonian),
                Request::List(SourceId::NytDaily),
            ]
        );
        // Going back and in again keeps the listing.
        keys(&mut app, "q3");
        assert!(app.take_requests().is_empty());
    }

    #[test]
    fn browse_switches_tabs_and_shows_failures() {
        let mut app = App::new(None);
        app.open_size(Size::Mini);
        app.on_response(
            Response::Listed(SourceId::NytMini, Err(SourceError::NeedsCookie)),
            Instant::now(),
        );
        keys(&mut app, "l");
        let browse = app.browse.as_ref().unwrap();
        assert_eq!(browse.tab().source, SourceId::NytMini);
        assert!(matches!(browse.tab().listing, Listing::Failed(ref e) if e.contains("NYT_S")));
        keys(&mut app, "l"); // wraps
        assert_eq!(app.browse.as_ref().unwrap().active, 0);
    }

    #[test]
    fn stale_downloads_are_ignored() {
        let mut app = App::new(None);
        app.open_size(Size::Crossword);
        app.on_response(
            Response::Fetched(reference(), Ok(small().data().clone())),
            Instant::now(),
        );
        assert_eq!(app.screen, Screen::Browse);
    }

    #[test]
    fn insert_mode_types_and_escape_returns_to_normal() {
        let mut app = solving();
        keys(&mut app, "icat");
        assert_eq!(solve(&app).mode, Mode::Insert);
        assert_eq!(grid(&app), "CAT...#..");
        keys(&mut app, "⎋");
        assert_eq!(solve(&app).mode, Mode::Normal);
        // In normal mode, letters are commands, not answers.
        keys(&mut app, "hjkl");
        assert_eq!(grid(&app), "CAT...#..");
    }

    #[test]
    fn alt_chord_in_insert_mode_is_escape_then_the_key() {
        let mut app = solving();
        keys(&mut app, "ic");
        app.handle_key(
            KeyEvent::new(KeyCode::Char('q'), KeyModifiers::ALT),
            Instant::now(),
        );
        // Esc left insert mode, and q then left the puzzle.
        assert_eq!(app.screen, Screen::Browse);
        let item = app.browse.as_ref().unwrap().selected_item().unwrap();
        assert_eq!(item.status, Status::Started);
    }

    #[test]
    fn insert_mode_autosaves() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::new(dir.path(), dir.path());
        let mut app = solving_with(Some(store.clone()));
        let start = Instant::now();
        app.tick(start); // nothing typed yet
        assert_eq!(store.status(&reference()), Status::New);
        keys(&mut app, "ic");
        app.tick(start + AUTOSAVE);
        assert_eq!(store.status(&reference()), Status::Started);
        assert_eq!(solve(&app).mode, Mode::Insert);
    }

    #[test]
    fn ctrl_c_leaves_insert_mode_but_not_the_puzzle() {
        let mut app = solving();
        keys(&mut app, "i");
        ctrl_key(&mut app, 'c');
        assert_eq!(solve(&app).mode, Mode::Normal);
        ctrl_key(&mut app, 'c');
        assert_eq!(app.screen, Screen::Solve);
        assert!(!app.should_quit);
        assert!(app.message.as_ref().unwrap().text.contains(":q"));
    }

    #[test]
    fn hjkl_move_and_counts_repeat() {
        let mut app = solving();
        keys(&mut app, "l");
        assert_eq!(solve(&app).game.cursor(), 1);
        keys(&mut app, "j");
        assert_eq!(solve(&app).game.cursor(), 4);
        keys(&mut app, "k2h");
        assert_eq!(solve(&app).game.cursor(), 0);
        keys(&mut app, "2w"); // 1A -> 4A -> 5A
        assert_eq!(solve(&app).game.cursor(), 7);
        keys(&mut app, "gg");
        assert_eq!(solve(&app).game.cursor(), 0);
    }

    #[test]
    fn operators_clear_entries_and_undo_restores() {
        let mut app = solving();
        keys(&mut app, "icatare⎋");
        keys(&mut app, "ggdd");
        assert_eq!(grid(&app), "...ARE#..");
        keys(&mut app, "u");
        assert_eq!(grid(&app), "CATARE#..");
        ctrl_key(&mut app, 'r');
        assert_eq!(grid(&app), "...ARE#..");
        keys(&mut app, "jcwXY⎋"); // cw from the start of 4A
        assert_eq!(grid(&app), "...XY.#..");
        keys(&mut app, "u");
        assert_eq!(grid(&app), "...ARE#..");
    }

    #[test]
    fn r_and_x_edit_one_square() {
        let mut app = solving();
        keys(&mut app, "rqlrz");
        assert_eq!(grid(&app), "QZ....#..");
        assert_eq!(solve(&app).game.cursor(), 1);
        keys(&mut app, "x");
        assert_eq!(grid(&app), "Q.....#..");
    }

    #[test]
    fn rebus_mode_fills_one_square_with_several_letters() {
        let mut app = solving();
        keys(&mut app, "Rstar⏎");
        assert_eq!(solve(&app).game.letter(0), "STAR");
        assert_eq!(solve(&app).mode, Mode::Normal);
    }

    #[test]
    fn commands_check_reveal_and_jump() {
        let mut app = solving();
        keys(&mut app, "icot⎋");
        keys(&mut app, ":check puzzle⏎");
        assert_eq!(solve(&app).game.mark(1), Mark::Wrong);
        assert_eq!(app.message.as_ref().unwrap().tone, Tone::Error);
        keys(&mut app, ":2d⏎");
        assert_eq!(
            (solve(&app).game.cursor(), solve(&app).game.direction()),
            (1, Direction::Down)
        );
        keys(&mut app, ":reveal cell⏎");
        assert_eq!(solve(&app).game.letter(1), "A");
        keys(&mut app, ":frobnicate⏎");
        assert!(
            app.message
                .as_ref()
                .unwrap()
                .text
                .contains("Not an editor command")
        );
        keys(&mut app, ":check sideways⏎");
        assert_eq!(app.message.as_ref().unwrap().tone, Tone::Error);
    }

    #[test]
    fn backspace_on_an_empty_command_line_cancels_it() {
        let mut app = solving();
        keys(&mut app, ":⌫");
        assert_eq!(solve(&app).mode, Mode::Normal);
    }

    #[test]
    fn solving_announces_and_locks() {
        let mut app = solving();
        keys(&mut app, "icatareee");
        assert!(solve(&app).game.is_solved());
        assert_eq!(solve(&app).mode, Mode::Normal);
        assert_eq!(app.message.as_ref().unwrap().tone, Tone::Success);
        keys(&mut app, "i");
        assert_eq!(solve(&app).mode, Mode::Normal);
    }

    #[test]
    fn a_full_wrong_grid_says_so() {
        let mut app = solving();
        keys(&mut app, "icatareex");
        assert!(
            app.message
                .as_ref()
                .unwrap()
                .text
                .contains("something is wrong")
        );
    }

    #[test]
    fn q_leaves_and_records_status() {
        let mut app = solving();
        keys(&mut app, "ic⎋q");
        assert_eq!(app.screen, Screen::Browse);
        assert!(app.solve.is_none());
        let item = app.browse.as_ref().unwrap().selected_item().unwrap();
        assert_eq!(item.status, Status::Started);
    }

    #[test]
    fn losing_focus_pauses_the_timer() {
        let mut app = solving();
        let t0 = Instant::now();
        app.set_focus(false, t0);
        assert!(!solve(&app).game.is_running());
        app.set_focus(true, t0);
        assert!(solve(&app).game.is_running());
    }

    #[test]
    fn qa_quits_from_the_puzzle() {
        let mut app = solving();
        keys(&mut app, ":qa⏎");
        assert!(app.should_quit);
    }

    #[test]
    fn progress_persists_across_sessions() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::new(dir.path(), dir.path());
        let mut app = App::new(Some(store.clone()));
        app.open_size(Size::Crossword);
        app.on_response(
            Response::Listed(SourceId::Universal, Ok(vec![reference()])),
            Instant::now(),
        );
        keys(&mut app, "⏎");
        app.on_response(
            Response::Fetched(reference(), Ok(small().data().clone())),
            Instant::now(),
        );
        keys(&mut app, "icat⎋:q⏎");
        assert_eq!(store.status(&reference()), Status::Started);

        keys(&mut app, "⏎");
        app.on_response(
            Response::Fetched(reference(), Ok(small().data().clone())),
            Instant::now(),
        );
        assert_eq!(grid(&app), "CAT...#..");
    }

    #[test]
    fn help_scrolls_with_j_and_k_and_other_keys_close_it() {
        let mut app = solving();
        keys(&mut app, "?jjk");
        assert!(app.show_help);
        assert_eq!(app.help_scroll, 1);
        keys(&mut app, "l");
        assert!(!app.show_help);
        assert_eq!(solve(&app).game.cursor(), 0); // the key only closed help
    }

    #[test]
    fn showcmd_reflects_pending_keys() {
        let mut app = solving();
        keys(&mut app, "3");
        assert_eq!(solve(&app).showcmd(), "3");
        keys(&mut app, "d");
        assert_eq!(solve(&app).showcmd(), "d");
        keys(&mut app, "⎋");
        assert_eq!(solve(&app).showcmd(), "");
    }

    #[test]
    fn durations_format_like_a_clock() {
        assert_eq!(format_duration(Duration::from_secs(5)), "0:05");
        assert_eq!(format_duration(Duration::from_secs(754)), "12:34");
        assert_eq!(format_duration(Duration::from_secs(3723)), "1:02:03");
    }

    #[test]
    fn clue_refs_parse() {
        assert_eq!(parse_clue_ref("12a"), Some((12, Some(Direction::Across))));
        assert_eq!(parse_clue_ref("3D"), Some((3, Some(Direction::Down))));
        assert_eq!(parse_clue_ref("7"), Some((7, None)));
        assert_eq!(parse_clue_ref("abc"), None);
        assert_eq!(parse_clue_ref("a"), None);
    }
}
