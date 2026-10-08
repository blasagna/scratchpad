//! The key reference that both frontends show for `?`.

/// One group of keys, such as "Normal mode".
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Section {
    pub title: &'static str,
    /// Keys and what they do. An empty key column continues the line above.
    pub rows: &'static [(&'static str, &'static str)],
}

pub const NORMAL: Section = Section {
    title: "Normal mode",
    rows: &[
        ("h j k l  arrows", "move, over blocks; counts work: 3l"),
        ("w  b  e", "next entry / its start / its end"),
        ("0  ^  $", "start / end of this entry"),
        ("gg  G", "first / last square"),
        ("Tab  Shift-Tab", "next / previous unfinished entry"),
        ("space  Enter", "switch across / down"),
        ("i  a", "insert here / on the next square"),
        ("I  A", "insert at the start / end of entry"),
        ("x  r{c}  R", "clear / replace a square / rebus"),
        ("dd  D", "clear the entry / to its end"),
        ("cc  C  s", "change the entry / to end / square"),
        ("u  Ctrl-r", "undo / redo"),
        (":", "command line"),
        ("q", "back to the list (progress is saved)"),
    ],
};

pub const INSERT: Section = Section {
    title: "Insert mode",
    rows: &[
        ("letters", "type and advance"),
        ("Backspace", "erase, or step back and erase"),
        ("space", "erase and step on"),
        ("arrows  Tab", "move / next unfinished entry"),
        ("Enter", "switch across / down"),
        ("Esc  Ctrl-c", "back to normal mode"),
    ],
};

pub const COMMANDS: Section = Section {
    title: "Commands",
    rows: &[
        (":check [scope]", "mark wrong letters"),
        (":reveal [scope]", "show answers"),
        (":clear [scope]", "erase letters"),
        ("", "scope: cell, word or puzzle"),
        ("", "(word if left out)"),
        (":12a  :12d  :12", "jump to a clue"),
        (":reset", "start over, timer too"),
        (":w  :q  :qa", "save / back / quit"),
    ],
};

pub const LISTS: Section = Section {
    title: "Lists",
    rows: &[
        ("j k  g G", "move"),
        ("Ctrl-d  Ctrl-u", "move a page"),
        ("h l  Tab", "switch source"),
        ("Enter  r", "open / reload"),
        ("q", "back"),
    ],
};

/// The sections as two columns: editing on the left, the rest on the right.
pub const COLUMNS: [&[Section]; 2] = [&[NORMAL, INSERT], &[COMMANDS, LISTS]];

/// Lines in the help as one column: each section's title, its rows and a
/// blank line. This bounds how far the help can scroll until a frontend
/// reports the limit of its own view.
pub fn line_count() -> u16 {
    COLUMNS
        .iter()
        .flat_map(|column| column.iter())
        .map(|s| s.rows.len() as u16 + 2)
        .sum()
}
