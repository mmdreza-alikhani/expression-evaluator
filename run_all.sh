#!/usr/bin/env bash
# Build and test all three implementations on Linux/macOS.
set -u

cd "$(dirname "$0")"
mkdir -p bin

CC="${CC:-cc}"
PYTHON="${PYTHON:-python3}"

echo "== build C =="
$CC -std=c11 -O2 -Wall -Wextra -o bin/evaluator c/evaluator.c -lm || exit 1

echo "== build Go =="
(cd go && go build -o ../bin/evaluator-go .) || exit 1

echo
echo "== Python tests =="
"$PYTHON" python/evaluator.py --test tests/cases.txt
py_status=$?

echo
echo "== Go tests =="
./bin/evaluator-go -test -cases tests/cases.txt
go_status=$?

echo
echo "== C tests =="
./bin/evaluator --test tests/cases.txt
c_status=$?

echo
echo "== summary =="
printf 'python: %s\n' "$([ $py_status -eq 0 ] && echo PASS || echo FAIL)"
printf 'go:     %s\n' "$([ $go_status -eq 0 ] && echo PASS || echo FAIL)"
printf 'c:      %s\n' "$([ $c_status -eq 0 ] && echo PASS || echo FAIL)"

[ $py_status -eq 0 ] && [ $go_status -eq 0 ] && [ $c_status -eq 0 ]
