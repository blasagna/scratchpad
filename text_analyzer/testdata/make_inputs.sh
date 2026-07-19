#!/usr/bin/env bash
#
# Generate the golden-test input files.
#
# The inputs are committed — this script exists so the awkward ones (binary
# bytes, CRLF, missing trailing newline) are reviewable as printf escapes rather
# than as opaque bytes. Run it only when adding or changing a case, then run
# ./regenerate.sh to refresh the expected outputs.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

# Zero bytes: empty rankings and zeroed word-length stats.
printf '' > empty.txt

# Trailing word with no newline: the final-flush path, and a final line that is
# not counted as a line at all.
printf 'solo' > single_word.txt

# An empty line and a whitespace-only line, both blank.
printf 'a\n\n  \nb\n' > blank_lines.txt

# CRLF endings: '\r' is whitespace but not a letter, so "\r\n" is a blank line.
printf 'alpha beta\r\n\r\ngamma\r\n' > crlf.txt

# Equal counts for both words and characters, exercising both tie-breaks.
printf 'bb aa cc aa bb cc\ndd dd\n' > ties.txt

# Case folding: all four spellings collapse to one word.
printf 'The THE tHe the\nMiXeD mixed\n' > mixed_case.txt

# Digits and punctuation, including runs and adjacency to words.
printf 'abc 123, def! 45.67 -- x?\n' > digits_punct.txt

# Word lengths 1,2,3,4,5,6,7,8 so nearest-rank quantile boundaries are exact.
printf 'a bb ccc dddd eeeee ffffff ggggggg hhhhhhhh\n' > quantiles.txt

# Truncation under the alt config (max_word_len=5), plus one word longer than
# the 256-bucket length histogram to exercise clamping.
{
  printf 'short truncateme alsotruncated\n'
  printf 'x'
  for _ in $(seq 300); do printf 'y'; done
  printf '\n'
} > long_words.txt

# '"' and '\' reach the top-chars ranking, exercising the JSON escape path.
printf 'a "b" \\c\\ "d" \\e\\ "f"\n' > json_escape.txt

# UTF-8 under the ASCII contract: each byte of a multi-byte sequence counts as a
# character, is not a letter, and separates words.
printf 'caf\xc3\xa9 na\xc3\xafve stra\xc3\x9fe\n' > non_ascii.txt

# NUL and control bytes, which are neither letters, digits, nor punctuation.
printf 'a\x00b\x01c\x1fd\x7fe\x80f\xff\n' > binary.txt

# Exactly 32 characters with 't' appearing 5 times, so one frequency is exactly
# 5/32 = 0.15625 — a halfway case at the fourth decimal. printf("%.4f") and
# Rust's {:.4} both round half to even and give 0.1562; rounding half away from
# zero gives 0.1563. Without a case like this, that divergence goes unnoticed.
printf 'the cat sat.\n\n  \non the mat 42!\n' > rounding_ties.txt

echo "wrote $(wc -l < cases.txt) inputs" >&2
