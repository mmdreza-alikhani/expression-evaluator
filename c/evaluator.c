/* Expression evaluation with stacks (C11).
 *
 * Pipeline: text -> tokens -> postfix (shunting-yard) -> value (RPN stack)
 *
 * Build (MSVC):  cl /nologo /std:c11 /O2 /W4 /Fe:evaluator.exe evaluator.c
 * Build (GCC):   gcc -std=c11 -O2 -Wall -Wextra -o evaluator evaluator.c -lm
 *
 * Usage:
 *   evaluator "3 + 4 * (2 - 1)"
 *   evaluator --test [tests/cases.txt]
 *   evaluator --bench bench/data/expr_100k.txt
 */
#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define strcasecmp _stricmp
#if defined(_DEBUG)
#include <crtdbg.h>
#endif
#else
#include <strings.h>
#endif

/* ------------------------------------------------------------------ types */

typedef enum { TOK_NUM = 0, TOK_OP, TOK_LPAREN, TOK_RPAREN } TokKind;

typedef struct {
    TokKind kind;
    double value;
    char op;
} Token;

typedef struct {
    Token *items;
    size_t len;
    size_t cap;
} TokenVec;

typedef struct {
    double *items;
    size_t len;
    size_t cap;
} NumStack;

typedef struct {
    char *items;
    size_t len;
    size_t cap;
} CharStack;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

typedef struct {
    TokenVec tokens;
    TokenVec postfix;
    NumStack values;
    CharStack ops;
    StrBuf out;
    char err[256];
} Evaluator;

/* ----------------------------------------------------------------- errors */

static int fail(Evaluator *ev, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev->err, sizeof ev->err, fmt, ap);
    va_end(ap);
    return -1;
}

/* -------------------------------------------------------------- allocator */

static int ensure_capacity(void **data, size_t *cap, size_t need, size_t elem_size)
{
    size_t new_cap;
    void *p;

    if (need <= *cap) {
        return 0;
    }
    new_cap = (*cap == 0) ? 16 : *cap;
    while (new_cap < need) {
        if (new_cap > ((size_t)-1) / 2) {
            return -1;
        }
        new_cap *= 2;
    }
    if (new_cap > ((size_t)-1) / elem_size) {
        return -1;
    }
    p = realloc(*data, new_cap * elem_size);
    if (p == NULL) {
        return -1;
    }
    *data = p;
    *cap = new_cap;
    return 0;
}

static int push_token(Evaluator *ev, TokenVec *v, Token t)
{
    if (ensure_capacity((void **)&v->items, &v->cap, v->len + 1, sizeof(Token)) != 0) {
        return fail(ev, "out of memory");
    }
    v->items[v->len++] = t;
    return 0;
}

static int push_num(Evaluator *ev, NumStack *s, double value)
{
    if (ensure_capacity((void **)&s->items, &s->cap, s->len + 1, sizeof(double)) != 0) {
        return fail(ev, "out of memory");
    }
    s->items[s->len++] = value;
    return 0;
}

static int push_op(Evaluator *ev, CharStack *s, char op)
{
    if (ensure_capacity((void **)&s->items, &s->cap, s->len + 1, sizeof(char)) != 0) {
        return fail(ev, "out of memory");
    }
    s->items[s->len++] = op;
    return 0;
}

static int sb_putc(Evaluator *ev, StrBuf *sb, char c)
{
    if (ensure_capacity((void **)&sb->data, &sb->cap, sb->len + 2, sizeof(char)) != 0) {
        return fail(ev, "out of memory");
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
    return 0;
}

static int sb_puts(Evaluator *ev, StrBuf *sb, const char *s)
{
    size_t n = strlen(s);
    if (ensure_capacity((void **)&sb->data, &sb->cap, sb->len + n + 1, sizeof(char)) != 0) {
        return fail(ev, "out of memory");
    }
    memcpy(sb->data + sb->len, s, n + 1);
    sb->len += n;
    return 0;
}

/* --------------------------------------------------------------- tokenizer */

/* Exact fast path for numbers with <= 15 significant digits and <= 22 decimals:
 * mantissa and 10^frac are both exactly representable, and IEEE-754 division
 * is correctly rounded, so the result is bit-identical to strtod(). */
static double parse_number(const char *buf, size_t len)
{
    static const double pow10[23] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    unsigned long long mantissa = 0;
    int significant = 0;
    int frac_digits = 0;
    int seen_dot = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '.') {
            seen_dot = 1;
            continue;
        }
        mantissa = mantissa * 10u + (unsigned)(c - '0');
        significant++;
        if (seen_dot) {
            frac_digits++;
        }
    }
    if (significant <= 15 && frac_digits <= 22) {
        return (double)mantissa / pow10[frac_digits];
    }
    return strtod(buf, NULL);
}

static int tokenize(Evaluator *ev, const char *s, TokenVec *out)
{
    size_t i = 0;
    size_t n = strlen(s);

    out->len = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            i++;
            continue;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            size_t start = i;
            size_t int_digits = 0;
            size_t frac_digits = 0;
            int has_dot = 0;
            char buf[64];
            size_t len;
            Token t;

            while (i < n && s[i] >= '0' && s[i] <= '9') {
                i++;
                int_digits++;
            }
            if (i < n && s[i] == '.') {
                has_dot = 1;
                i++;
                while (i < n && s[i] >= '0' && s[i] <= '9') {
                    i++;
                    frac_digits++;
                }
            }
            if (int_digits == 0 && frac_digits == 0) {
                return fail(ev, "invalid number at position %lu", (unsigned long)(start + 1));
            }
            if (has_dot && frac_digits == 0) {
                return fail(ev, "invalid number '%.*s': decimal point must be followed by a digit",
                            (int)(i - start), s + start);
            }
            len = i - start;
            if (len >= sizeof buf) {
                return fail(ev, "number too long at position %lu", (unsigned long)(start + 1));
            }
            memcpy(buf, s + start, len);
            buf[len] = '\0';

            t.kind = TOK_NUM;
            t.value = parse_number(buf, len);
            t.op = 0;
            if (push_token(ev, out, t) != 0) {
                return -1;
            }
            continue;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/') {
            Token t;
            t.kind = TOK_OP;
            t.value = 0.0;
            t.op = (char)c;
            if (push_token(ev, out, t) != 0) {
                return -1;
            }
            i++;
            continue;
        }
        if (c == '(') {
            Token t;
            t.kind = TOK_LPAREN;
            t.value = 0.0;
            t.op = '\0';
            if (push_token(ev, out, t) != 0) {
                return -1;
            }
            i++;
            continue;
        }
        if (c == ')') {
            Token t;
            t.kind = TOK_RPAREN;
            t.value = 0.0;
            t.op = '\0';
            if (push_token(ev, out, t) != 0) {
                return -1;
            }
            i++;
            continue;
        }
        return fail(ev, "invalid character '%c' at position %lu", (char)c,
                    (unsigned long)(i + 1));
    }
    return 0;
}

/* ------------------------------------------------------------ shunting-yard */

static int precedence(char op)
{
    switch (op) {
    case '+':
    case '-':
        return 1;
    case '*':
    case '/':
        return 2;
    case 'u': /* unary minus */
        return 3;
    default: /* '(' */
        return 0;
    }
}

static int to_postfix(Evaluator *ev, const TokenVec *tokens, TokenVec *out)
{
    int expect_operand = 1;
    size_t k;

    out->len = 0;
    ev->ops.len = 0;

    for (k = 0; k < tokens->len; k++) {
        Token t = tokens->items[k];
        switch (t.kind) {
        case TOK_NUM:
            if (!expect_operand) {
                return fail(ev, "missing operator between operands");
            }
            if (push_token(ev, out, t) != 0) {
                return -1;
            }
            expect_operand = 0;
            break;

        case TOK_LPAREN:
            if (!expect_operand) {
                return fail(ev, "missing operator before '('");
            }
            if (push_op(ev, &ev->ops, '(') != 0) {
                return -1;
            }
            expect_operand = 1;
            break;

        case TOK_RPAREN:
            if (expect_operand) {
                return fail(ev, "empty parentheses or missing operand before ')'");
            }
            while (ev->ops.len > 0 && ev->ops.items[ev->ops.len - 1] != '(') {
                Token op;
                op.kind = TOK_OP;
                op.value = 0.0;
                op.op = ev->ops.items[--ev->ops.len];
                if (push_token(ev, out, op) != 0) {
                    return -1;
                }
            }
            if (ev->ops.len == 0) {
                return fail(ev, "mismatched parentheses: unexpected ')'");
            }
            ev->ops.len--; /* drop '(' */
            expect_operand = 0;
            break;

        default: /* operator */
            if (expect_operand) {
                if (t.op == '-') {
                    if (push_op(ev, &ev->ops, 'u') != 0) { /* right-assoc */
                        return -1;
                    }
                } else if (t.op == '+') {
                    /* unary plus: no-op */
                } else {
                    return fail(ev, "unexpected operator '%c'", t.op);
                }
            } else {
                while (ev->ops.len > 0 && ev->ops.items[ev->ops.len - 1] != '(' &&
                       precedence(ev->ops.items[ev->ops.len - 1]) >= precedence(t.op)) {
                    Token op;
                    op.kind = TOK_OP;
                    op.value = 0.0;
                    op.op = ev->ops.items[--ev->ops.len];
                    if (push_token(ev, out, op) != 0) {
                        return -1;
                    }
                }
                if (push_op(ev, &ev->ops, t.op) != 0) {
                    return -1;
                }
                expect_operand = 1;
            }
            break;
        }
    }

    if (expect_operand) {
        return fail(ev, "incomplete expression: missing operand");
    }
    while (ev->ops.len > 0) {
        char op = ev->ops.items[--ev->ops.len];
        Token t;
        if (op == '(') {
            return fail(ev, "mismatched parentheses: missing ')'");
        }
        t.kind = TOK_OP;
        t.value = 0.0;
        t.op = op;
        if (push_token(ev, out, t) != 0) {
            return -1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- evaluator */

static int eval_postfix(Evaluator *ev, const TokenVec *postfix, double *result)
{
    size_t k;
    ev->values.len = 0;

    for (k = 0; k < postfix->len; k++) {
        Token t = postfix->items[k];
        if (t.kind == TOK_NUM) {
            if (push_num(ev, &ev->values, t.value) != 0) {
                return -1;
            }
            continue;
        }
        if (t.op == 'u') {
            if (ev->values.len < 1) {
                return fail(ev, "malformed expression: missing operand for unary minus");
            }
            ev->values.items[ev->values.len - 1] = -ev->values.items[ev->values.len - 1];
            continue;
        }
        if (ev->values.len < 2) {
            return fail(ev, "malformed expression: missing operands");
        }
        {
            double b = ev->values.items[--ev->values.len];
            double a = ev->values.items[ev->values.len - 1];
            switch (t.op) {
            case '+':
                ev->values.items[ev->values.len - 1] = a + b;
                break;
            case '-':
                ev->values.items[ev->values.len - 1] = a - b;
                break;
            case '*':
                ev->values.items[ev->values.len - 1] = a * b;
                break;
            case '/':
                if (b == 0.0) {
                    return fail(ev, "division by zero");
                }
                ev->values.items[ev->values.len - 1] = a / b;
                break;
            default:
                return fail(ev, "unknown operator '%c'", t.op);
            }
        }
    }
    if (ev->values.len != 1) {
        return fail(ev, "malformed expression: unbalanced operands");
    }
    *result = ev->values.items[0];
    return 0;
}

/* ----------------------------------------------------------------- output */

static void format_number(char *buf, size_t size, double value)
{
    snprintf(buf, size, "%.10g", value);
}

static int build_postfix_string(Evaluator *ev, const TokenVec *postfix, StrBuf *sb)
{
    size_t k;
    char num[48];

    sb->len = 0;
    if (sb->data != NULL) {
        sb->data[0] = '\0';
    }
    for (k = 0; k < postfix->len; k++) {
        Token t = postfix->items[k];
        if (k > 0 && sb_putc(ev, sb, ' ') != 0) {
            return -1;
        }
        if (t.kind == TOK_NUM) {
            format_number(num, sizeof num, t.value);
            if (sb_puts(ev, sb, num) != 0) {
                return -1;
            }
        } else if (t.op == 'u') {
            if (sb_putc(ev, sb, 'u') != 0) {
                return -1;
            }
        } else {
            if (sb_putc(ev, sb, t.op) != 0) {
                return -1;
            }
        }
    }
    if (sb->data == NULL) {
        if (sb_putc(ev, sb, '\0') != 0) { /* ensure empty string is allocated */
            return -1;
        }
        sb->len = 0;
        sb->data[0] = '\0';
    }
    return 0;
}

/* ---------------------------------------------------------------- pipeline */

static int evaluate_text(Evaluator *ev, const char *text, double *value)
{
    if (tokenize(ev, text, &ev->tokens) != 0) {
        return -1;
    }
    if (to_postfix(ev, &ev->tokens, &ev->postfix) != 0) {
        return -1;
    }
    if (eval_postfix(ev, &ev->postfix, value) != 0) {
        return -1;
    }
    return 0;
}

static void evaluator_free(Evaluator *ev)
{
    free(ev->tokens.items);
    free(ev->postfix.items);
    free(ev->values.items);
    free(ev->ops.items);
    free(ev->out.data);
    ev->tokens.items = NULL;
    ev->postfix.items = NULL;
    ev->values.items = NULL;
    ev->ops.items = NULL;
    ev->out.data = NULL;
    ev->tokens.len = ev->postfix.len = ev->values.len = ev->ops.len = 0;
    ev->out.len = 0;
}

/* ------------------------------------------------------------------ timer */

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized = 0;
    LARGE_INTEGER counter;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

/* ------------------------------------------------------------------- file */

static char *read_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    size_t got;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (out_size != NULL) {
        *out_size = (long)got;
    }
    return buf;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' || *s == '\v' || *s == '\f') {
        s++;
    }
    end = s + strlen(s);
    while (end > s) {
        char c = end[-1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            end--;
        } else {
            break;
        }
    }
    *end = '\0';
    return s;
}

/* ------------------------------------------------------------------ modes */

static int nearly_equal(double a, double b)
{
    double diff = fabs(a - b);
    double scale;
    if (diff <= 1e-9) {
        return 1;
    }
    scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return diff <= 1e-9 * scale;
}

static int run_tests(Evaluator *ev, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[8192];
    int passed = 0;
    int total = 0;

    if (f == NULL) {
        fprintf(stderr, "error: cannot open %s\n", path);
        return 2;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char *expr;
        char *bar;
        char *expected;
        double value = 0.0;
        int want_error;

        {
            char *p = trim(line);
            if (*p == '\0' || *p == '#') {
                continue;
            }
            bar = strchr(p, '|');
            if (bar == NULL) {
                fprintf(stderr, "error: %s: expected 'expression|expected' in %s\n", path, p);
                fclose(f);
                return 2;
            }
            expr = p;
            *bar = '\0';
            expected = trim(bar + 1);
            expr = trim(expr);
        }

        total++;
        want_error = (strcasecmp(expected, "ERROR") == 0);
        ev->tokens.len = 0;
        ev->postfix.len = 0;

        if (evaluate_text(ev, expr, &value) != 0) {
            if (want_error) {
                passed++;
                printf("[PASS] %-24s | ERROR: %s\n", expr, ev->err);
            } else {
                printf("[FAIL] %-24s | ERROR: %s | expected %s\n", expr, ev->err, expected);
            }
            continue;
        }
        if (want_error) {
            printf("[FAIL] %-24s | expected ERROR, got %.10g\n", expr, value);
            continue;
        }
        if (nearly_equal(value, strtod(expected, NULL))) {
            char num[48];
            passed++;
            format_number(num, sizeof num, value);
            if (build_postfix_string(ev, &ev->postfix, &ev->out) != 0) {
                fprintf(stderr, "error: %s\n", ev->err);
                fclose(f);
                return 2;
            }
            printf("[PASS] %-24s | postfix: %s | result: %s\n", expr, ev->out.data, num);
        } else {
            printf("[FAIL] %-24s | expected %s, got %.10g\n", expr, expected, value);
        }
    }
    fclose(f);
    printf("RESULT lang=c passed=%d total=%d\n", passed, total);
    return passed == total ? 0 : 1;
}

static int run_bench(Evaluator *ev, const char *path)
{
    char *text;
    char *body;
    double value = 0.0;
    double times[10];
    double sum = 0.0;
    double minimum, maximum;
    int i;
    size_t token_count;

    text = read_file(path, NULL);
    if (text == NULL) {
        fprintf(stderr, "error: cannot read %s\n", path);
        return 2;
    }
    body = trim(text);

    if (evaluate_text(ev, body, &value) != 0) {
        fprintf(stderr, "error: %s\n", ev->err);
        free(text);
        return 2;
    }
    token_count = ev->tokens.len;

    for (i = 0; i < 10; i++) {
        double start = now_ms();
        double v = 0.0;
        if (evaluate_text(ev, body, &v) != 0) {
            fprintf(stderr, "error: %s\n", ev->err);
            free(text);
            return 2;
        }
        times[i] = now_ms() - start;
        sum += times[i];
    }
    minimum = times[0];
    maximum = times[0];
    for (i = 1; i < 10; i++) {
        if (times[i] < minimum) {
            minimum = times[i];
        }
        if (times[i] > maximum) {
            maximum = times[i];
        }
    }
    printf("bench lang=c tokens=%lu avg_ms=%.3f min_ms=%.3f max_ms=%.3f result=%.10g\n",
           (unsigned long)token_count, sum / 10.0, minimum, maximum, value);
    free(text);
    return 0;
}

/* Works both from the repository root and from the c/ directory. */
static const char *default_cases_path(void)
{
    FILE *f = fopen("../tests/cases.txt", "r");
    if (f != NULL) {
        fclose(f);
        return "../tests/cases.txt";
    }
    return "tests/cases.txt";
}

int main(int argc, char **argv)
{
    Evaluator ev;
    int status = 0;

#if defined(_DEBUG)
    /* Debug CRT reports any block that is still allocated at exit. */
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    memset(&ev, 0, sizeof ev);

    if (argc < 2) {
        printf("usage: evaluator \"<expression>\" | --test [cases.txt] | --bench <file>\n");
        return 2;
    }
    if (strcmp(argv[1], "--test") == 0) {
        status = run_tests(&ev, argc > 2 ? argv[2] : default_cases_path());
    } else if (strcmp(argv[1], "--bench") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: evaluator --bench <expression-file>\n");
            status = 2;
        } else {
            status = run_bench(&ev, argv[2]);
        }
    } else {
        double value = 0.0;
        if (evaluate_text(&ev, argv[1], &value) != 0) {
            fprintf(stderr, "error: %s\n", ev.err);
            status = 1;
        } else {
            char num[48];
            format_number(num, sizeof num, value);
            if (build_postfix_string(&ev, &ev.postfix, &ev.out) != 0) {
                fprintf(stderr, "error: %s\n", ev.err);
                status = 2;
            } else {
                printf("postfix: %s\nresult: %s\n", ev.out.data, num);
            }
        }
    }
    evaluator_free(&ev);
    return status;
}
