CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
PYTHON ?= python3
BIN := bin

.PHONY: all test bench asan clean

all: $(BIN)/evaluator $(BIN)/evaluator-go

$(BIN):
	mkdir -p $(BIN)

$(BIN)/evaluator: c/evaluator.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -lm

$(BIN)/evaluator-go: go/main.go go/go.mod | $(BIN)
	cd go && go build -o ../$(BIN)/evaluator-go .

test: all
	$(PYTHON) python/evaluator.py --test tests/cases.txt
	$(BIN)/evaluator-go -test -cases tests/cases.txt
	$(BIN)/evaluator --test tests/cases.txt

bench: all
	$(PYTHON) bench/gen_cases.py
	$(PYTHON) bench/run_bench.py

asan: | $(BIN)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -o $(BIN)/evaluator-asan c/evaluator.c -lm
	$(BIN)/evaluator-asan --test tests/cases.txt

clean:
	rm -rf $(BIN) bench/data
