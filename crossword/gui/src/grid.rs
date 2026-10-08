//! The puzzle grid as an iced canvas: NYT colours, clue numbers in the
//! corners, and a click on a square sends [`Message::Cell`].

use iced::alignment;
use iced::font::{self, Font};
use iced::mouse;
use iced::widget::canvas::{self, Action, Event, Frame, Geometry, Path, Stroke, Text};
use iced::{Point, Rectangle, Renderer, Size, Theme, Vector};

use crossword_core::game::{Game, Mark};

use crate::Message;
use crate::style::{
    BLOCK, CORRECT, CURSOR_INSERT, CURSOR_NORMAL, INK, LINES, NUMBER, PAPER, REVEALED, WORD, WRONG,
};

/// Space kept around the grid, in logical pixels.
const MARGIN: f32 = 8.0;
/// Squares grow with the window up to this size.
const MAX_CELL: f32 = 120.0;

/// Where the squares sit inside the canvas.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Placement {
    pub origin: Point,
    pub cell: f32,
    pub width: usize,
    pub height: usize,
}

impl Placement {
    /// The largest whole-pixel squares that fit, centred across the canvas
    /// and set at its top.
    pub fn new(bounds: Size, width: usize, height: usize) -> Placement {
        let fit_w = (bounds.width - 2.0 * MARGIN) / width as f32;
        let fit_h = (bounds.height - 2.0 * MARGIN) / height as f32;
        let cell = fit_w.min(fit_h).clamp(4.0, MAX_CELL).floor();
        let used = cell * width as f32;
        Placement {
            origin: Point::new(((bounds.width - used) / 2.0).max(0.0).floor(), MARGIN),
            cell,
            width,
            height,
        }
    }

    /// The square under a point in canvas coordinates.
    pub fn cell_at(&self, point: Point) -> Option<usize> {
        let x = (point.x - self.origin.x) / self.cell;
        let y = (point.y - self.origin.y) / self.cell;
        let inside = x >= 0.0 && y >= 0.0 && x < self.width as f32 && y < self.height as f32;
        inside.then(|| y as usize * self.width + x as usize)
    }

    fn top_left(&self, cell: usize) -> Point {
        Point::new(
            self.origin.x + (cell % self.width) as f32 * self.cell,
            self.origin.y + (cell / self.width) as f32 * self.cell,
        )
    }
}

/// The grid of one game, as drawn on a canvas.
pub struct Grid<'a> {
    pub game: &'a Game,
    /// Insert mode colours the cursor green instead of yellow.
    pub insert: bool,
}

impl Grid<'_> {
    fn placement(&self, bounds: Size) -> Placement {
        let puzzle = self.game.puzzle();
        Placement::new(bounds, puzzle.width(), puzzle.height())
    }
}

impl canvas::Program<Message> for Grid<'_> {
    type State = ();

    fn update(
        &self,
        _state: &mut (),
        event: &Event,
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> Option<Action<Message>> {
        let Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) = event else {
            return None;
        };
        let point = cursor.position_in(bounds)?;
        let cell = self.placement(bounds.size()).cell_at(point)?;
        self.game
            .puzzle()
            .is_open(cell)
            .then(|| Action::publish(Message::Cell(cell)).and_capture())
    }

    fn draw(
        &self,
        _state: &(),
        renderer: &Renderer,
        _theme: &Theme,
        bounds: Rectangle,
        _cursor: mouse::Cursor,
    ) -> Vec<Geometry> {
        let mut frame = Frame::new(renderer, bounds.size());
        let game = self.game;
        let puzzle = game.puzzle();
        let place = self.placement(bounds.size());
        let size = place.cell;
        let full = Size::new(size * place.width as f32, size * place.height as f32);
        let word = &puzzle.entry(game.current_entry()).cells;
        let cursor_colour = if self.insert {
            CURSOR_INSERT
        } else {
            CURSOR_NORMAL
        };

        // Blocks are the background; open squares are painted over it.
        frame.fill_rectangle(place.origin, full, BLOCK);
        for cell in puzzle.open_cells() {
            let colour = if cell == game.cursor() {
                cursor_colour
            } else if word.contains(&cell) {
                WORD
            } else {
                PAPER
            };
            frame.fill_rectangle(place.top_left(cell), Size::new(size, size), colour);
        }

        let thin = Stroke::default().with_width(1.0).with_color(LINES);
        for row in 1..place.height {
            let y = place.origin.y + row as f32 * size;
            let line = Path::line(
                Point::new(place.origin.x, y),
                Point::new(place.origin.x + full.width, y),
            );
            frame.stroke(&line, thin);
        }
        for col in 1..place.width {
            let x = place.origin.x + col as f32 * size;
            let line = Path::line(
                Point::new(x, place.origin.y),
                Point::new(x, place.origin.y + full.height),
            );
            frame.stroke(&line, thin);
        }
        frame.stroke(
            &Path::rectangle(place.origin, full),
            Stroke::default().with_width(2.0).with_color(BLOCK),
        );

        let bold = Font {
            weight: font::Weight::Bold,
            ..Font::DEFAULT
        };
        for cell in puzzle.open_cells() {
            let corner = place.top_left(cell);
            let centre = corner + Vector::new(size / 2.0, size / 2.0);
            if puzzle.is_circled(cell) {
                let ring = Path::circle(centre, size * 0.46);
                frame.stroke(&ring, Stroke::default().with_width(1.0).with_color(NUMBER));
            }
            if let Some(number) = puzzle.number(cell) {
                frame.fill_text(Text {
                    content: number.to_string(),
                    position: corner + Vector::new(size * 0.06, size * 0.04),
                    size: (size * 0.28).max(7.0).into(),
                    color: NUMBER,
                    ..Text::default()
                });
            }

            let letters = game.letter(cell);
            if !letters.is_empty() {
                let colour = match game.mark(cell) {
                    Mark::Wrong => WRONG,
                    Mark::Revealed => REVEALED,
                    Mark::Correct => CORRECT,
                    Mark::None => INK,
                };
                // A rebus shrinks to fit its letters into the square.
                let scale = match letters.chars().count() {
                    1 => 0.58,
                    2 => 0.42,
                    3 => 0.3,
                    _ => 0.22,
                };
                frame.fill_text(Text {
                    content: letters.to_string(),
                    position: centre + Vector::new(0.0, size * 0.1),
                    size: (size * scale).into(),
                    color: colour,
                    font: bold,
                    align_x: iced::widget::text::Alignment::Center,
                    align_y: alignment::Vertical::Center,
                    ..Text::default()
                });
            }

            match game.mark(cell) {
                // A slash marks a wrong letter, as in the NYT app.
                Mark::Wrong => {
                    let slash = Path::line(
                        corner + Vector::new(size * 0.9, size * 0.1),
                        corner + Vector::new(size * 0.1, size * 0.9),
                    );
                    frame.stroke(&slash, Stroke::default().with_width(1.5).with_color(WRONG));
                }
                // A corner triangle marks a revealed square.
                Mark::Revealed => {
                    let tri = Path::new(|p| {
                        p.move_to(corner + Vector::new(size * 0.72, 0.0));
                        p.line_to(corner + Vector::new(size, 0.0));
                        p.line_to(corner + Vector::new(size, size * 0.28));
                        p.close();
                    });
                    frame.fill(&tri, REVEALED);
                }
                Mark::Correct | Mark::None => {}
            }
        }
        vec![frame.into_geometry()]
    }

    fn mouse_interaction(
        &self,
        _state: &(),
        bounds: Rectangle,
        cursor: mouse::Cursor,
    ) -> mouse::Interaction {
        let over_square = cursor
            .position_in(bounds)
            .and_then(|p| self.placement(bounds.size()).cell_at(p))
            .is_some_and(|c| self.game.puzzle().is_open(c));
        if over_square {
            mouse::Interaction::Pointer
        } else {
            mouse::Interaction::default()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn squares_fit_the_canvas_and_clicks_find_them() {
        let place = Placement::new(Size::new(316.0, 400.0), 5, 5);
        assert_eq!(place.cell, 60.0); // (316 - 16) / 5
        assert_eq!(place.origin, Point::new(8.0, 8.0));
        assert_eq!(place.cell_at(Point::new(9.0, 9.0)), Some(0));
        assert_eq!(
            place.cell_at(Point::new(8.0 + 60.0 * 4.5, 8.0 + 60.0 * 1.5)),
            Some(9)
        );
        assert_eq!(place.cell_at(Point::new(2.0, 2.0)), None);
        assert_eq!(place.cell_at(Point::new(8.0 + 300.0, 20.0)), None);
    }

    #[test]
    fn squares_stop_growing_and_centre() {
        let place = Placement::new(Size::new(1000.0, 1000.0), 5, 5);
        assert_eq!(place.cell, MAX_CELL);
        assert_eq!(
            place.origin.x,
            ((1000.0 - 5.0 * MAX_CELL) / 2.0_f32).floor()
        );
    }
}
