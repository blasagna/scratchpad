//! The desktop frontend: an iced GUI over the shared `crossword_core`.
//!
//! [`Gui`] holds the core [`App`] and forwards every message to it. Key
//! presses become core keys and run the same vim-style handling as the TUI;
//! clicks call the core's pointer actions. The views only draw the core's
//! state, and downloads go to the core's [`Fetcher`] on threads of their own.

pub mod grid;
pub mod input;
pub mod style;
mod view;

use std::sync::Arc;
use std::time::{Duration, Instant};

use chrono::Local;
use iced::futures::channel::oneshot;
use iced::widget::operation::{RelativeOffset, snap_to};
use iced::{Element, Subscription, Task, Theme, event, keyboard, window};

use crossword_core::app::{App, Screen};
use crossword_core::keys::Key;
use crossword_core::sources::{Fetcher, Request, Response, Size, SourceError};

/// Scrollable ids, so updates can bring the selection into view.
pub const ACROSS_LIST: &str = "across";
pub const DOWN_LIST: &str = "down";
pub const PUZZLE_LIST: &str = "puzzles";

/// How often the solve timer redraws.
const TICK: Duration = Duration::from_millis(500);

#[derive(Debug, Clone)]
pub enum Message {
    /// A key press, already in the core's terms.
    Key(Key),
    /// The timer: redraws the clock and lets the core autosave.
    Tick,
    Focus(bool),
    /// The window's close button: save first, then exit.
    CloseRequested,
    /// A finished download. Boxed: it can hold a whole puzzle, and every
    /// other message is small.
    Fetched(Box<Response>),
    Size(Size),
    Tab(usize),
    Item(usize),
    Back,
    Reload,
    Cell(usize),
    Clue(usize),
    Help,
    CloseHelp,
}

/// What the lists last scrolled to show. They move only when it changes, so
/// they never fight the player's own scrolling.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ScrollTarget {
    Clues {
        active: usize,
        crossing: Option<usize>,
    },
    Puzzles {
        tab: usize,
        selected: usize,
    },
}

pub struct Gui {
    app: App,
    /// `None` drops requests; tests answer them by hand instead.
    fetcher: Option<Arc<Fetcher>>,
    scrolled: Option<ScrollTarget>,
}

impl Gui {
    pub fn new(app: App, fetcher: Option<Fetcher>) -> Gui {
        Gui {
            app,
            fetcher: fetcher.map(Arc::new),
            scrolled: None,
        }
    }

    pub fn app(&self) -> &App {
        &self.app
    }

    /// Sends the requests the core has made so far, such as the listings of
    /// a size chosen on the command line.
    pub fn flush(&mut self) -> Task<Message> {
        let requests: Vec<Request> = self.app.take_requests();
        Task::batch(requests.into_iter().filter_map(|r| self.fetch(r)))
    }

    pub fn update(&mut self, message: Message) -> Task<Message> {
        let now = Instant::now();
        let app = &mut self.app;
        match message {
            Message::Key(key) => app.handle_key(key, now),
            Message::Tick => app.tick(now),
            Message::Focus(focused) => app.set_focus(focused, now),
            Message::CloseRequested => app.quit(now),
            Message::Fetched(response) => app.on_response(*response, now),
            Message::Size(size) => app.open_size(size),
            Message::Tab(index) => app.select_tab(index),
            Message::Item(index) => app.open_item(index),
            Message::Back => app.back(now),
            Message::Reload => app.reload(),
            Message::Cell(cell) => app.click_cell(cell),
            Message::Clue(entry) => app.click_clue(entry),
            Message::Help => app.open_help(),
            Message::CloseHelp => app.close_help(),
        }
        let mut tasks = vec![self.flush()];
        tasks.extend(self.scroll());
        if self.app.should_quit {
            tasks.push(iced::exit());
        }
        Task::batch(tasks)
    }

    pub fn view(&self) -> Element<'_, Message> {
        view::view(&self.app, Instant::now())
    }

    pub fn subscription(&self) -> Subscription<Message> {
        let events = event::listen_with(|event, _status, _window| match event {
            iced::Event::Keyboard(keyboard::Event::KeyPressed {
                key,
                modified_key,
                modifiers,
                ..
            }) => input::key(&key, &modified_key, modifiers).map(Message::Key),
            iced::Event::Window(window::Event::Focused) => Some(Message::Focus(true)),
            iced::Event::Window(window::Event::Unfocused) => Some(Message::Focus(false)),
            iced::Event::Window(window::Event::CloseRequested) => Some(Message::CloseRequested),
            _ => None,
        });
        let timing = self.app.solve.as_ref().is_some_and(|s| s.game.is_running());
        if timing {
            Subscription::batch([events, iced::time::every(TICK).map(|_| Message::Tick)])
        } else {
            events
        }
    }

    pub fn title(&self) -> String {
        match &self.app.solve {
            Some(solve) if self.app.screen == Screen::Solve => {
                format!(
                    "{} {} · crossword",
                    solve.puzzle.source.name(),
                    solve.puzzle.label()
                )
            }
            _ => "crossword".to_string(),
        }
    }

    pub fn theme(&self) -> Theme {
        Theme::Light
    }

    /// Runs one request on a thread of its own: the core's fetcher blocks on
    /// the network, and the UI must not.
    fn fetch(&self, request: Request) -> Option<Task<Message>> {
        let fetcher = Arc::clone(self.fetcher.as_ref()?);
        let (sender, receiver) = oneshot::channel();
        let fallback = request.clone();
        std::thread::spawn(move || {
            let _ = sender.send(fetcher.handle(request, Local::now().date_naive()));
        });
        Some(Task::perform(receiver, move |answer| {
            Message::Fetched(Box::new(answer.unwrap_or_else(|_| {
                fallback
                    .clone()
                    .failed(SourceError::Network("the download thread failed".into()))
            })))
        }))
    }

    /// Brings the current clue and the crossing clue, or the selected
    /// puzzle, into view after they change.
    fn scroll(&mut self) -> Vec<Task<Message>> {
        let target = match self.app.screen {
            Screen::Solve => self.app.solve.as_ref().map(|s| ScrollTarget::Clues {
                active: s.game.current_entry(),
                crossing: s.game.crossing_entry(),
            }),
            Screen::Browse => self.app.browse.as_ref().map(|b| ScrollTarget::Puzzles {
                tab: b.active,
                selected: b.tab().selected,
            }),
            Screen::Home => None,
        };
        if target.is_none() || target == self.scrolled {
            return Vec::new();
        }
        self.scrolled = target;

        // A relative offset of i / (n - 1) always shows row i of n equal rows.
        let fraction =
            |index: usize, count: usize| index as f32 / count.saturating_sub(1).max(1) as f32;
        match target {
            Some(ScrollTarget::Clues { active, crossing }) => {
                let puzzle = self
                    .app
                    .solve
                    .as_ref()
                    .map(|s| s.game.puzzle())
                    .expect("solving");
                [Some(active), crossing]
                    .into_iter()
                    .flatten()
                    .map(|entry| {
                        let direction = puzzle.entry(entry).direction;
                        let list: Vec<usize> = (0..puzzle.entries().len())
                            .filter(|&e| puzzle.entry(e).direction == direction)
                            .collect();
                        let index = list.iter().position(|&e| e == entry).unwrap_or(0);
                        let id = match direction {
                            crossword_core::puzzle::Direction::Across => ACROSS_LIST,
                            crossword_core::puzzle::Direction::Down => DOWN_LIST,
                        };
                        snap_to(
                            id,
                            RelativeOffset {
                                x: 0.0,
                                y: fraction(index, list.len()),
                            },
                        )
                    })
                    .collect()
            }
            Some(ScrollTarget::Puzzles { selected, .. }) => {
                let count = self
                    .app
                    .browse
                    .as_ref()
                    .map_or(0, |b| b.tab().items().len());
                vec![snap_to(
                    PUZZLE_LIST,
                    RelativeOffset {
                        x: 0.0,
                        y: fraction(selected, count),
                    },
                )]
            }
            None => Vec::new(),
        }
    }
}
