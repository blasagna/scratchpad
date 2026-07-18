#!/usr/bin/env python3
"""Generate a reproducible corpus for text_analyzer benchmarks.

Vocabulary size is the interesting variable: a word table with per-word linear
scan costs O(words * distinct), so holding the file size and word count fixed
while varying --distinct isolates lookup cost from I/O cost.
"""

import argparse
import random

WORDS_PER_LINE = 12


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--words", type=int, default=400_000,
                        help="total words to emit (default: 400000)")
    parser.add_argument("--distinct", type=int, default=40_000,
                        help="size of the vocabulary (default: 40000)")
    parser.add_argument("--seed", type=int, default=7,
                        help="RNG seed, for reproducibility (default: 7)")
    parser.add_argument("--out", required=True, help="output file path")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    alphabet = "abcdefghijklmnopqrstuvwxyz"
    vocabulary = [
        "".join(rng.choices(alphabet, k=rng.randint(3, 9)))
        for _ in range(args.distinct)
    ]

    with open(args.out, "w", encoding="ascii") as f:
        for i in range(args.words):
            f.write(rng.choice(vocabulary))
            f.write("\n" if i % WORDS_PER_LINE == WORDS_PER_LINE - 1 else " ")


if __name__ == "__main__":
    main()
