#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Expression evaluation with stacks.

Pipeline:  text -> tokens -> postfix (shunting-yard) -> value (RPN stack)

Usage:
    python evaluator.py "3 + 4 * (2 - 1)"
    python evaluator.py --test [tests/cases.txt]
    python evaluator.py --bench bench/data/expr_100k.txt
"""

from __future__ import annotations

import math
import sys
import time
from pathlib import Path

PRECEDENCE = {"+": 1, "-": 1, "*": 2, "/": 2, "(": 0}
UNARY_MINUS = "u"
UNARY_PRECEDENCE = 3
DIGITS = "0123456789"
RUNS = 10


class ExprError(Exception):
    """Invalid expression: syntax error, bad character or math error."""


# --------------------------------------------------------------------------
# 1. Tokenizer
# --------------------------------------------------------------------------
def tokenize(text: str):
    tokens = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch in " \t\r\n\v\f":
            i += 1
            continue
        if ch in DIGITS or ch == ".":
            start = i
            int_digits = 0
            while i < n and text[i] in DIGITS:
                i += 1
                int_digits += 1
            frac_digits = 0
            has_dot = False
            if i < n and text[i] == ".":
                has_dot = True
                i += 1
                while i < n and text[i] in DIGITS:
                    i += 1
                    frac_digits += 1
            if int_digits == 0 and frac_digits == 0:
                raise ExprError("invalid number at position %d" % (start + 1))
            if has_dot and frac_digits == 0:
                raise ExprError(
                    "invalid number %r: decimal point must be followed by a digit"
                    % text[start:i]
                )
            tokens.append(("num", float(text[start:i])))
            continue
        if ch in "+-*/()":
            tokens.append((ch, None))
            i += 1
            continue
        raise ExprError("invalid character %r at position %d" % (ch, i + 1))
    return tokens


# --------------------------------------------------------------------------
# 2. Shunting-yard: infix -> postfix
# --------------------------------------------------------------------------
def prec(op: str) -> int:
    return UNARY_PRECEDENCE if op == UNARY_MINUS else PRECEDENCE[op]


def to_postfix(tokens):
    output = []
    ops = []
    expect_operand = True
    for kind, value in tokens:
        if kind == "num":
            if not expect_operand:
                raise ExprError("missing operator between operands")
            output.append(("num", value))
            expect_operand = False
        elif kind == "(":
            if not expect_operand:
                raise ExprError("missing operator before '('")
            ops.append("(")
            expect_operand = True
        elif kind == ")":
            if expect_operand:
                raise ExprError("empty parentheses or missing operand before ')'")
            while ops and ops[-1] != "(":
                output.append(("op", ops.pop()))
            if not ops:
                raise ExprError("mismatched parentheses: unexpected ')'")
            ops.pop()
            expect_operand = False
        else:  # binary or unary +/- * /
            if expect_operand:
                if kind == "-":
                    ops.append(UNARY_MINUS)  # right-associative, highest precedence
                elif kind == "+":
                    pass  # unary plus is a no-op
                else:
                    raise ExprError("unexpected operator %r" % kind)
            else:
                while (
                    ops
                    and ops[-1] != "("
                    and prec(ops[-1]) >= PRECEDENCE[kind]  # left-associative
                ):
                    output.append(("op", ops.pop()))
                ops.append(kind)
                expect_operand = True
    if expect_operand:
        raise ExprError("incomplete expression: missing operand")
    while ops:
        op = ops.pop()
        if op == "(":
            raise ExprError("mismatched parentheses: missing ')'")
        output.append(("op", op))
    return output


# --------------------------------------------------------------------------
# 3. RPN evaluation with a stack
# --------------------------------------------------------------------------
def eval_postfix(postfix):
    stack = []
    for kind, value in postfix:
        if kind == "num":
            stack.append(value)
            continue
        if value == UNARY_MINUS:
            if not stack:
                raise ExprError("malformed expression: missing operand for unary minus")
            stack[-1] = -stack[-1]
            continue
        if len(stack) < 2:
            raise ExprError("malformed expression: missing operands")
        b = stack.pop()
        a = stack.pop()
        if value == "+":
            stack.append(a + b)
        elif value == "-":
            stack.append(a - b)
        elif value == "*":
            stack.append(a * b)
        else:
            if b == 0.0:
                raise ExprError("division by zero")
            stack.append(a / b)
    if len(stack) != 1:
        raise ExprError("malformed expression: unbalanced operands")
    return stack[0]


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
def fmt(value: float) -> str:
    """Canonical number format shared with the Go and C implementations."""
    return format(value, ".10g")


def postfix_to_string(postfix) -> str:
    parts = []
    for kind, value in postfix:
        if kind == "num":
            parts.append(fmt(value))
        elif value == UNARY_MINUS:
            parts.append("u")
        else:
            parts.append(value)
    return " ".join(parts)


def evaluate(text: str):
    postfix = to_postfix(tokenize(text))
    return postfix_to_string(postfix), eval_postfix(postfix)


def load_cases(path: Path):
    cases = []
    text = path.read_text(encoding="utf-8")
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "|" not in line:
            raise SystemExit("%s:%d: expected 'expression|expected'" % (path, lineno))
        expr, expected = line.split("|", 1)
        cases.append((expr.strip(), expected.strip()))
    if not cases:
        raise SystemExit("%s: no test cases found" % path)
    return cases


def parse_expected(expected: str):
    if expected.upper() == "ERROR":
        return None
    try:
        return float(expected)
    except ValueError:
        raise SystemExit("invalid expected value: %r" % expected)


def run_tests(path: Path) -> int:
    cases = load_cases(path)
    passed = 0
    for expr, expected in cases:
        want = parse_expected(expected)
        try:
            postfix_str, value = evaluate(expr)
            got = fmt(value)
            ok = want is not None and math.isclose(
                value, want, rel_tol=1e-9, abs_tol=1e-9
            )
            detail = "postfix: %s | result: %s" % (postfix_str, got)
        except ExprError as exc:
            got = "ERROR: %s" % exc
            ok = want is None
            detail = got
        if ok:
            passed += 1
            print("[PASS] %-24s | %s" % (expr, detail))
        else:
            print("[FAIL] %-24s | %s | expected %s, got %s"
                  % (expr, detail, expected, got))
    total = len(cases)
    print("RESULT lang=python passed=%d total=%d" % (passed, total))
    return 0 if passed == total else 1


def run_bench(path: Path) -> int:
    text = path.read_text(encoding="utf-8").strip()
    tokens = tokenize(text)
    value = eval_postfix(to_postfix(tokens))
    times = []
    for _ in range(RUNS):
        start = time.perf_counter()
        postfix = to_postfix(tokenize(text))
        eval_postfix(postfix)
        times.append((time.perf_counter() - start) * 1000.0)
    print(
        "bench lang=python tokens=%d avg_ms=%.3f min_ms=%.3f max_ms=%.3f result=%s"
        % (len(tokens), sum(times) / RUNS, min(times), max(times), fmt(value))
    )
    return 0


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__.strip())
        return 2

    here = Path(__file__).resolve().parent
    arg = argv[1]

    if arg == "--test":
        path = Path(argv[2]) if len(argv) > 2 else here.parent / "tests" / "cases.txt"
        return run_tests(path)

    if arg == "--bench":
        if len(argv) < 3:
            print("usage: evaluator.py --bench <expression-file>", file=sys.stderr)
            return 2
        return run_bench(Path(argv[2]))

    try:
        postfix_str, value = evaluate(arg)
    except ExprError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    print("postfix: %s" % postfix_str)
    print("result: %s" % fmt(value))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
