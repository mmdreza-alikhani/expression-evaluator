#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate large, valid, deterministic random expressions for the benchmark.

The generator builds a sum of parenthesised products:

    (1.25*3+? ) ... -> (a*b/c) + (d*e) - (f*g*h) ...

Every number is non-zero, so division can never fail, and each term is small
enough that the total value stays in a sane double range.
"""

from __future__ import annotations

import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"


def number(rng: random.Random, allow_negative: bool = False) -> str:
    if rng.random() < 0.35:
        text = str(rng.randint(1, 19))
    else:
        text = "%.2f" % rng.uniform(0.5, 9.5)
    if allow_negative and rng.random() < 0.05:
        text = "-" + text
    return text


def term(rng: random.Random):
    """Return (text, token_count) for one parenthesised product/quotient."""
    parts = ["("]
    count = 1
    if rng.random() < 0.06:
        parts.append("-")  # unary minus on the first factor
        count += 1
    parts.append(number(rng))
    count += 1
    for _ in range(rng.randint(1, 4)):
        op = "*" if rng.random() < 0.7 else "/"
        parts.append(op)
        parts.append(number(rng))
        count += 2
    parts.append(")")
    count += 1
    return "".join(parts), count


def generate(target_tokens: int, seed: int) -> str:
    rng = random.Random(seed)
    chunks = []
    count = 0
    while count < target_tokens:
        text, tokens = term(rng)
        chunks.append(text)
        count += tokens
        if count < target_tokens:
            op = "+" if rng.random() < 0.5 else "-"
            chunks.append(op)
            count += 1
    return "".join(chunks)


def main() -> int:
    DATA.mkdir(parents=True, exist_ok=True)
    sizes = [("100k", 100_000, 20260214), ("1m", 1_000_000, 20260215)]
    for label, tokens, seed in sizes:
        path = DATA / ("expr_%s.txt" % label)
        text = generate(tokens, seed)
        path.write_text(text, encoding="utf-8")
        print("%s: %d chars, ~%d tokens" % (path.name, len(text), tokens))
    return 0


if __name__ == "__main__":
    sys.exit(main())
