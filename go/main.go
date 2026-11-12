// Expression evaluation with stacks: tokenizer -> shunting-yard -> RPN evaluator.
//
// Usage:
//
//	go run . "3 + 4 * (2 - 1)"
//	go run . -test ../tests/cases.txt
//	go run . -bench ../bench/data/expr_100k.txt
package main

import (
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"strconv"
	"strings"
	"time"
)

// ---------------------------------------------------------------- tokens

type kind int

const (
	kNum kind = iota
	kOp
	kLParen
	kRParen
)

type token struct {
	kind  kind
	value float64
	op    byte
}

type evalError struct{ msg string }

func (e *evalError) Error() string { return e.msg }

func errf(format string, args ...any) error {
	return &evalError{msg: fmt.Sprintf(format, args...)}
}

// ---------------------------------------------------------------- stacks
// Hand-written stacks over a dynamic array; no container/stack helpers.

type numStack struct{ data []float64 }

func (s *numStack) push(v float64) { s.data = append(s.data, v) }

func (s *numStack) pop() (float64, bool) {
	n := len(s.data)
	if n == 0 {
		return 0, false
	}
	v := s.data[n-1]
	s.data = s.data[:n-1]
	return v, true
}

type opStack struct{ data []byte }

func (s *opStack) push(op byte) { s.data = append(s.data, op) }

func (s *opStack) pop() (byte, bool) {
	n := len(s.data)
	if n == 0 {
		return 0, false
	}
	op := s.data[n-1]
	s.data = s.data[:n-1]
	return op, true
}

func (s *opStack) top() (byte, bool) {
	n := len(s.data)
	if n == 0 {
		return 0, false
	}
	return s.data[n-1], true
}

// ---------------------------------------------------------------- tokenizer

func tokenize(s string) ([]token, error) {
	out := make([]token, 0, len(s)/2+1)
	i := 0
	for i < len(s) {
		c := s[i]
		switch {
		case c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f':
			i++
		case c >= '0' && c <= '9' || c == '.':
			start := i
			intDigits := 0
			for i < len(s) && s[i] >= '0' && s[i] <= '9' {
				i++
				intDigits++
			}
			fracDigits := 0
			hasDot := false
			if i < len(s) && s[i] == '.' {
				hasDot = true
				i++
				for i < len(s) && s[i] >= '0' && s[i] <= '9' {
					i++
					fracDigits++
				}
			}
			if intDigits == 0 && fracDigits == 0 {
				return nil, errf("invalid number at position %d", start+1)
			}
			if hasDot && fracDigits == 0 {
				return nil, errf("invalid number %q: decimal point must be followed by a digit", s[start:i])
			}
			v, err := strconv.ParseFloat(s[start:i], 64)
			if err != nil {
				return nil, errf("invalid number %q", s[start:i])
			}
			out = append(out, token{kind: kNum, value: v})
		case c == '+' || c == '-' || c == '*' || c == '/':
			out = append(out, token{kind: kOp, op: c})
			i++
		case c == '(':
			out = append(out, token{kind: kLParen})
			i++
		case c == ')':
			out = append(out, token{kind: kRParen})
			i++
		default:
			return nil, errf("invalid character %q at position %d", c, i+1)
		}
	}
	return out, nil
}

// ---------------------------------------------------------------- shunting-yard

const unaryMinus = 'u'

func precedence(op byte) int {
	switch op {
	case '+', '-':
		return 1
	case '*', '/':
		return 2
	case unaryMinus:
		return 3
	default: // '('
		return 0
	}
}

func toPostfix(tokens []token) ([]token, error) {
	out := make([]token, 0, len(tokens))
	var ops opStack
	expectOperand := true

	for _, t := range tokens {
		switch t.kind {
		case kNum:
			if !expectOperand {
				return nil, errf("missing operator between operands")
			}
			out = append(out, t)
			expectOperand = false
		case kLParen:
			if !expectOperand {
				return nil, errf("missing operator before '('")
			}
			ops.push('(')
			expectOperand = true
		case kRParen:
			if expectOperand {
				return nil, errf("empty parentheses or missing operand before ')'")
			}
			for {
				top, ok := ops.top()
				if !ok || top == '(' {
					break
				}
				ops.pop()
				out = append(out, token{kind: kOp, op: top})
			}
			if _, ok := ops.pop(); !ok {
				return nil, errf("mismatched parentheses: unexpected ')'")
			}
			expectOperand = false
		default: // operator
			if expectOperand {
				switch t.op {
				case '-':
					ops.push(unaryMinus) // right-associative, highest precedence
				case '+':
					// unary plus: no-op
				default:
					return nil, errf("unexpected operator %q", string(t.op))
				}
				continue
			}
			for {
				top, ok := ops.top()
				if !ok || top == '(' || precedence(top) < precedence(t.op) {
					break
				}
				ops.pop()
				out = append(out, token{kind: kOp, op: top})
			}
			ops.push(t.op)
			expectOperand = true
		}
	}
	if expectOperand {
		return nil, errf("incomplete expression: missing operand")
	}
	for {
		top, ok := ops.pop()
		if !ok {
			break
		}
		if top == '(' {
			return nil, errf("mismatched parentheses: missing ')'")
		}
		out = append(out, token{kind: kOp, op: top})
	}
	return out, nil
}

// ---------------------------------------------------------------- evaluator

func evalPostfix(postfix []token) (float64, error) {
	var st numStack
	for _, t := range postfix {
		if t.kind == kNum {
			st.push(t.value)
			continue
		}
		if t.op == unaryMinus {
			v, ok := st.pop()
			if !ok {
				return 0, errf("malformed expression: missing operand for unary minus")
			}
			st.push(-v)
			continue
		}
		b, ok := st.pop()
		if !ok {
			return 0, errf("malformed expression: missing operands")
		}
		a, ok := st.pop()
		if !ok {
			return 0, errf("malformed expression: missing operands")
		}
		switch t.op {
		case '+':
			st.push(a + b)
		case '-':
			st.push(a - b)
		case '*':
			st.push(a * b)
		case '/':
			if b == 0 {
				return 0, errf("division by zero")
			}
			st.push(a / b)
		default:
			return 0, errf("unknown operator %q", string(t.op))
		}
	}
	if len(st.data) != 1 {
		return 0, errf("malformed expression: unbalanced operands")
	}
	return st.data[0], nil
}

// ---------------------------------------------------------------- helpers

// format mirrors C's "%.10g" and Python's format(v, ".10g").
func format(v float64) string {
	return strconv.FormatFloat(v, 'g', 10, 64)
}

func postfixString(postfix []token) string {
	var b strings.Builder
	for i, t := range postfix {
		if i > 0 {
			b.WriteByte(' ')
		}
		switch {
		case t.kind == kNum:
			b.WriteString(format(t.value))
		case t.op == unaryMinus:
			b.WriteByte('u')
		default:
			b.WriteByte(t.op)
		}
	}
	return b.String()
}

func evaluate(expr string) (string, float64, error) {
	tokens, err := tokenize(expr)
	if err != nil {
		return "", 0, err
	}
	postfix, err := toPostfix(tokens)
	if err != nil {
		return "", 0, err
	}
	v, err := evalPostfix(postfix)
	if err != nil {
		return "", 0, err
	}
	return postfixString(postfix), v, nil
}

func nearlyEqual(a, b float64) bool {
	diff := math.Abs(a - b)
	if diff <= 1e-9 {
		return true
	}
	scale := math.Max(math.Abs(a), math.Abs(b))
	return diff <= 1e-9*scale
}

type testCase struct {
	expr     string
	expected string
}

func loadCases(path string) ([]testCase, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var cases []testCase
	for _, raw := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(strings.TrimSuffix(raw, "\r"))
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "|", 2)
		if len(parts) != 2 {
			return nil, errf("%s: expected 'expression|expected' in line %q", path, line)
		}
		cases = append(cases, testCase{
			expr:     strings.TrimSpace(parts[0]),
			expected: strings.TrimSpace(parts[1]),
		})
	}
	if len(cases) == 0 {
		return nil, errf("%s: no test cases found", path)
	}
	return cases, nil
}

// ---------------------------------------------------------------- modes

func runTests(path string) int {
	cases, err := loadCases(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 2
	}
	passed := 0
	for _, c := range cases {
		wantErr := strings.EqualFold(c.expected, "ERROR")
		postfix, value, err := evaluate(c.expr)
		if wantErr {
			if err != nil {
				passed++
				fmt.Printf("[PASS] %-24s | ERROR: %v\n", c.expr, err)
			} else {
				fmt.Printf("[FAIL] %-24s | expected ERROR, got %s\n", c.expr, format(value))
			}
			continue
		}
		if err != nil {
			fmt.Printf("[FAIL] %-24s | ERROR: %v | expected %s\n", c.expr, err, c.expected)
			continue
		}
		want, convErr := strconv.ParseFloat(c.expected, 64)
		if convErr != nil {
			fmt.Printf("[FAIL] %-24s | invalid expected value %q\n", c.expr, c.expected)
			continue
		}
		if nearlyEqual(value, want) {
			passed++
			fmt.Printf("[PASS] %-24s | postfix: %s | result: %s\n", c.expr, postfix, format(value))
		} else {
			fmt.Printf("[FAIL] %-24s | postfix: %s | expected %s, got %s\n",
				c.expr, postfix, c.expected, format(value))
		}
	}
	fmt.Printf("RESULT lang=go passed=%d total=%d\n", passed, len(cases))
	if passed == len(cases) {
		return 0
	}
	return 1
}

func runBench(path string) int {
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 2
	}
	text := strings.TrimSpace(string(data))
	tokens, err := tokenize(text)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 2
	}
	postfix, err := toPostfix(tokens)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 2
	}
	value, err := evalPostfix(postfix)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 2
	}

	const runs = 10
	times := make([]float64, 0, runs)
	for i := 0; i < runs; i++ {
		start := time.Now()
		toks, err := tokenize(text)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			return 2
		}
		pf, err := toPostfix(toks)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			return 2
		}
		if _, err := evalPostfix(pf); err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			return 2
		}
		times = append(times, float64(time.Since(start).Microseconds())/1000.0)
	}
	sum, minimum, maximum := 0.0, times[0], times[0]
	for _, t := range times {
		sum += t
		if t < minimum {
			minimum = t
		}
		if t > maximum {
			maximum = t
		}
	}
	fmt.Printf("bench lang=go tokens=%d avg_ms=%.3f min_ms=%.3f max_ms=%.3f result=%s\n",
		len(tokens), sum/runs, minimum, maximum, format(value))
	return 0
}

func usage() {
	fmt.Println("usage: evaluator \"<expression>\" | -test [-cases file] | -bench file")
}

func runExpression(expr string) int {
	postfix, value, err := evaluate(expr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 1
	}
	fmt.Printf("postfix: %s\nresult: %s\n", postfix, format(value))
	return 0
}

func main() {
	fs := flag.NewFlagSet("evaluator", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	testMode := fs.Bool("test", false, "run the shared test cases")
	casesPath := fs.String("cases", "", "path to cases.txt (default ../tests/cases.txt)")
	benchPath := fs.String("bench", "", "benchmark the expression stored in this file")

	if err := fs.Parse(os.Args[1:]); err != nil {
		if err == flag.ErrHelp {
			usage()
			os.Exit(0)
		}
		// A leading '-' may start a valid expression, e.g. "-3+4".
		if len(os.Args) >= 2 {
			os.Exit(runExpression(os.Args[1]))
		}
		usage()
		os.Exit(2)
	}

	if *casesPath == "" {
		if _, err := os.Stat("../tests/cases.txt"); err == nil {
			*casesPath = "../tests/cases.txt"
		} else {
			*casesPath = "tests/cases.txt"
		}
	}
	if *testMode {
		os.Exit(runTests(*casesPath))
	}
	if *benchPath != "" {
		os.Exit(runBench(*benchPath))
	}
	if fs.NArg() < 1 {
		usage()
		os.Exit(2)
	}
	os.Exit(runExpression(fs.Arg(0)))
}
