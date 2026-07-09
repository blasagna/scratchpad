//! Practice content for prompted mode.
//!
//! Two tracks are offered:
//!
//! - [`Track::Lessons`] — a progressive, Koch-style sequence that introduces
//!   letters in the dichotomic order used by [`crate::morse`] (E, T first, then
//!   I, A, N, M, …). Each lesson only uses letters introduced so far, so the
//!   learner builds up gradually.
//! - [`Track::Phrases`] — a mixed bag of short English phrases and sentences for
//!   learners who already know the alphabet.
//!
//! Every prompt uses only characters that [`crate::morse::encode`] recognizes
//! (lowercase letters, digits, and spaces); a test enforces this.

/// Which body of prompts to draw from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Track {
    Lessons,
    Phrases,
}

/// A named group of prompts that share a set of introduced letters.
#[derive(Debug, Clone, Copy)]
pub struct Lesson {
    pub title: &'static str,
    pub prompts: &'static [&'static str],
}

/// Progressive lessons, ordered to match the dichotomic letter progression.
pub static LESSONS: &[Lesson] = &[
    Lesson {
        title: "1. E T",
        prompts: &["e", "t", "et", "te", "tee", "tet", "ette"],
    },
    Lesson {
        title: "2. I A N M",
        prompts: &[
            "in", "an", "at", "am", "it", "tin", "tan", "man", "ant", "aim", "main", "mint",
            "neat", "meat", "team", "name", "mean",
        ],
    },
    Lesson {
        title: "3. S U R W D K G O",
        prompts: &[
            "sun", "run", "rat", "dog", "god", "word", "dark", "star", "rest", "road", "under",
            "water", "garden", "wonder", "dinosaur",
        ],
    },
    Lesson {
        title: "4. Full alphabet",
        prompts: &[
            "hello world",
            "morse code is fun",
            "how are you",
            "practice makes perfect",
            "the quick brown fox jumps over the lazy dog",
        ],
    },
    Lesson {
        title: "5. Numbers",
        prompts: &[
            "agent 007",
            "route 66",
            "5 gold rings",
            "channel 9 news",
            "the year 2024",
        ],
    },
];

/// A mixed set of short phrases for the free-practice track.
pub static PHRASES: &[&str] = &[
    "sos",
    "hello world",
    "the eagle has landed",
    "practice every day",
    "morse code is timeless",
    "dot dot dot dash dash dash",
    "keep calm and carry on",
    "the early bird gets the worm",
    "all that glitters is not gold",
    "a journey of a thousand miles begins with a single step",
];

/// Number of progressive lessons available.
pub fn lesson_count() -> usize {
    LESSONS.len()
}

/// Returns the prompt at position `index` within a track, wrapping around so the
/// caller can advance indefinitely. `lesson` selects the lesson for
/// [`Track::Lessons`] and is ignored for [`Track::Phrases`].
pub fn prompt(track: Track, lesson: usize, index: usize) -> &'static str {
    match track {
        Track::Lessons => {
            let prompts = LESSONS[lesson.min(LESSONS.len() - 1)].prompts;
            prompts[index % prompts.len()]
        }
        Track::Phrases => PHRASES[index % PHRASES.len()],
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::morse;

    #[test]
    fn tracks_are_non_empty() {
        assert!(!LESSONS.is_empty());
        assert!(!PHRASES.is_empty());
        for lesson in LESSONS {
            assert!(
                !lesson.prompts.is_empty(),
                "{} has no prompts",
                lesson.title
            );
        }
    }

    #[test]
    fn every_prompt_char_is_encodable() {
        let all_prompts = LESSONS
            .iter()
            .flat_map(|l| l.prompts.iter())
            .chain(PHRASES.iter());
        for prompt in all_prompts {
            for ch in prompt.chars() {
                assert!(
                    morse::encode(ch).is_some(),
                    "character {ch:?} in prompt {prompt:?} is not encodable",
                );
            }
        }
    }

    #[test]
    fn prompt_wraps_around() {
        let first = prompt(Track::Phrases, 0, 0);
        assert_eq!(prompt(Track::Phrases, 0, PHRASES.len()), first);
    }
}
