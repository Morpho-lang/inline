/** @file calc.c
 *  @brief Tiny colorful calculator demo for inline.
 */

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inline.h"

/* -----------------------
 * Palette indices (semantic)
 * ----------------------- */
enum {
    P_DEFAULT = 0,
    P_NUMBER,
    P_OPERATOR,
    P_PAREN,
    P_FUNCTION,
    P_IDENTIFIER, /* unknown identifier */
};

/* A small palette: map semantic indices -> terminal colors.
   You can swap these to taste. */
static const int g_palette[] = {
    [P_DEFAULT]    = -1,                        /* default terminal color */
    [P_NUMBER]     = INLINE_COLOR_ANSI216(1, 4, 2),   /* green-ish */
    [P_OPERATOR]   = INLINE_COLOR_ANSI216(5, 3, 1),   /* warm orange */
    [P_PAREN]      = INLINE_COLOR_ANSI216(2, 3, 5),   /* blue */
    [P_FUNCTION]   = INLINE_COLOR_ANSI216(4, 2, 5),   /* purple */
    [P_IDENTIFIER] = INLINE_COLOR_ANSI216(5, 1, 1),   /* red */
};

/* -----------------------
 * Autocomplete
 * ----------------------- */

static const char *k_words[] = {
    "sin", "cos", "tan", "pi", "e",
    "help", "quit",
};

static const char *completefn(const char *utf8, void *ref, size_t *index) {
    (void)ref;

    /* Complete the last "word-ish" token: [A-Za-z_][A-Za-z0-9_]* */
    size_t n = strlen(utf8);
    size_t start = n;
    while (start > 0) {
        unsigned char c = (unsigned char)utf8[start - 1];
        if (isalnum(c) || c == '_') start--;
        else break;
    }
    const char *partial = utf8 + start;
    size_t plen = strlen(partial);

    for (size_t i = *index; i < (sizeof(k_words) / sizeof(k_words[0])); i++) {
        const char *w = k_words[i];
        if (strncmp(w, partial, plen) == 0) {
            *index = i + 1;
            return w + plen; /* suffix only, per API */
        }
    }
    return NULL;
}

/* -----------------------
 * Syntax highlighting
 * ----------------------- */

static bool is_ident_start(unsigned char c) {
    return (isalpha(c) || c == '_');
}
static bool is_ident_cont(unsigned char c) {
    return (isalnum(c) || c == '_');
}

static int classify_identifier(const char *s, size_t len) {
    /* Highlight supported functions as FUNCTION; others as IDENTIFIER. */
    if (len == 3 && strncmp(s, "sin", 3) == 0) return P_FUNCTION;
    if (len == 3 && strncmp(s, "cos", 3) == 0) return P_FUNCTION;
    if (len == 3 && strncmp(s, "tan", 3) == 0) return P_FUNCTION;
    if (len == 2 && strncmp(s, "pi", 2) == 0)  return P_FUNCTION; /* treat constants as “function-ish” */
    if (len == 1 && strncmp(s, "e", 1) == 0)   return P_FUNCTION;
    if (len == 4 && strncmp(s, "quit", 4) == 0) return P_FUNCTION;
    if (len == 4 && strncmp(s, "help", 4) == 0) return P_FUNCTION;
    return P_IDENTIFIER;
}

static bool syntaxcolorfn(const char *utf8, void *ref, size_t offset, inline_colorspan_t *out) {
    (void)ref;

    const size_t n = strlen(utf8);
    if (offset >= n) return false;

    unsigned char c = (unsigned char)utf8[offset];

    /* Whitespace: emit as default (keeps callback simple and monotonic). */
    if (isspace(c)) {
        size_t i = offset + 1;
        while (i < n && isspace((unsigned char)utf8[i])) i++;
        out->byte_end = i;
        out->color = P_DEFAULT;
        return true;
    }

    /* Parentheses */
    if (c == '(' || c == ')') {
        out->byte_end = offset + 1;
        out->color = P_PAREN;
        return true;
    }

    /* Operators */
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == ',') {
        out->byte_end = offset + 1;
        out->color = P_OPERATOR;
        return true;
    }

    /* Number: [0-9]*('.'[0-9]+)?([eE][+-]?[0-9]+)? */
    if (isdigit(c) || c == '.') {
        size_t i = offset;
        bool saw_digit = false;

        while (i < n && isdigit((unsigned char)utf8[i])) { i++; saw_digit = true; }

        if (i < n && utf8[i] == '.') {
            i++;
            while (i < n && isdigit((unsigned char)utf8[i])) { i++; saw_digit = true; }
        }

        if (saw_digit && i < n && (utf8[i] == 'e' || utf8[i] == 'E')) {
            size_t j = i + 1;
            if (j < n && (utf8[j] == '+' || utf8[j] == '-')) j++;
            bool exp_digit = false;
            while (j < n && isdigit((unsigned char)utf8[j])) { j++; exp_digit = true; }
            if (exp_digit) i = j;
        }

        if (saw_digit) {
            out->byte_end = i;
            out->color = P_NUMBER;
            return true;
        }
        /* A lone '.' that isn’t a number -> default */
        out->byte_end = offset + 1;
        out->color = P_DEFAULT;
        return true;
    }

    /* Identifier */
    if (is_ident_start(c)) {
        size_t i = offset + 1;
        while (i < n && is_ident_cont((unsigned char)utf8[i])) i++;
        out->byte_end = i;
        out->color = classify_identifier(utf8 + offset, i - offset);
        return true;
    }

    /* Everything else: default */
    out->byte_end = offset + 1;
    out->color = P_DEFAULT;
    return true;
}

/* -----------------------
 * Multiline (unmatched '(' heuristic)
 * ----------------------- */

static bool multilinefn(const char *utf8, void *ref) {
    (void)ref;
    int depth = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
    }
    return (depth > 0);
}

/* -----------------------
 * Expression parser / evaluator
 * ----------------------- */

typedef struct {
    const char *s;
    const char *p;
    const char *err;
} parser_t;

static void skip_ws(parser_t *ps) {
    while (isspace((unsigned char)*ps->p)) ps->p++;
}

static bool match(parser_t *ps, char ch) {
    skip_ws(ps);
    if (*ps->p == ch) { ps->p++; return true; }
    return false;
}

static double parse_expr(parser_t *ps);

static double parse_number(parser_t *ps) {
    skip_ws(ps);
    char *end = NULL;
    double v = strtod(ps->p, &end);
    if (end == ps->p) {
        ps->err = "expected number";
        return 0.0;
    }
    ps->p = end;
    return v;
}

static bool parse_ident(parser_t *ps, char *buf, size_t buflen) {
    skip_ws(ps);
    const char *start = ps->p;
    if (!is_ident_start((unsigned char)*start)) return false;
    const char *q = start + 1;
    while (is_ident_cont((unsigned char)*q)) q++;

    size_t len = (size_t)(q - start);
    if (len + 1 > buflen) len = buflen - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    ps->p = q;
    return true;
}

static double parse_primary(parser_t *ps) {
    skip_ws(ps);

    /* Parenthesized expression */
    if (match(ps, '(')) {
        double v = parse_expr(ps);
        if (!ps->err && !match(ps, ')')) ps->err = "missing ')'";
        return v;
    }

    /* Identifier: function or constant */
    char ident[32];
    const char *save = ps->p;
    if (parse_ident(ps, ident, sizeof(ident))) {
        if (!strcmp(ident, "pi")) return 3.14159265358979323846;
        if (!strcmp(ident, "e"))  return 2.71828182845904523536;

        /* Function call: name '(' expr ')' */
        if (match(ps, '(')) {
            double arg = parse_expr(ps);
            if (!ps->err && !match(ps, ')')) ps->err = "missing ')' after function call";

            if (ps->err) return 0.0;

            if (!strcmp(ident, "sin")) return sin(arg);
            if (!strcmp(ident, "cos")) return cos(arg);
            if (!strcmp(ident, "tan")) return tan(arg);

            ps->err = "unknown function";
            return 0.0;
        }

        ps->err = "unknown identifier (did you mean sin(...), cos(...), tan(...), pi, e?)";
        return 0.0;
    }

    /* Number */
    ps->p = save;
    return parse_number(ps);
}

static double parse_unary(parser_t *ps) {
    skip_ws(ps);
    if (match(ps, '+')) return parse_unary(ps);
    if (match(ps, '-')) return -parse_unary(ps);
    return parse_primary(ps);
}

static double parse_power(parser_t *ps) {
    /* Right-associative: a ^ b ^ c = a ^ (b ^ c) */
    double left = parse_unary(ps);
    if (ps->err) return 0.0;

    skip_ws(ps);
    if (match(ps, '^')) {
        double right = parse_power(ps);
        if (ps->err) return 0.0;
        return pow(left, right);
    }
    return left;
}

static double parse_term(parser_t *ps) {
    double v = parse_power(ps);
    if (ps->err) return 0.0;

    for (;;) {
        if (match(ps, '*')) {
            double rhs = parse_power(ps);
            if (ps->err) return 0.0;
            v *= rhs;
        } else if (match(ps, '/')) {
            double rhs = parse_power(ps);
            if (ps->err) return 0.0;
            v /= rhs;
        } else {
            break;
        }
    }
    return v;
}

static double parse_expr(parser_t *ps) {
    double v = parse_term(ps);
    if (ps->err) return 0.0;

    for (;;) {
        if (match(ps, '+')) {
            double rhs = parse_term(ps);
            if (ps->err) return 0.0;
            v += rhs;
        } else if (match(ps, '-')) {
            double rhs = parse_term(ps);
            if (ps->err) return 0.0;
            v -= rhs;
        } else {
            break;
        }
    }
    return v;
}

static bool eval(const char *s, double *out, const char **err) {
    parser_t ps = {.s = s, .p = s, .err = NULL};

    double v = parse_expr(&ps);

    if (!ps.err) {
        skip_ws(&ps);
        if (*ps.p != '\0') ps.err = "unexpected trailing characters";
    }

    if (ps.err) {
        if (err) *err = ps.err;
        return false;
    }

    if (out) *out = v;
    return true;
}

/* -----------------------
 * Main REPL
 * ----------------------- */

static void print_help(void) {
    puts("Examples:");
    puts("  1 + 2*3");
    puts("  (1 + 2) * 3");
    puts("  2^8");
    puts("  sin(pi/2)");
    puts("  cos(0)");
    puts("  tan(pi/4)");
    puts("");
    puts("Commands: help, quit");
}

int main(void) {
    inline_editor *edit = inline_new("calc> ");
    if (!edit) return 1;

    inline_setpalette(edit, (int)(sizeof(g_palette) / sizeof(g_palette[0])), g_palette);
    inline_syntaxcolor(edit, syntaxcolorfn, NULL);
    inline_autocomplete(edit, completefn, NULL);
    inline_multiline(edit, multilinefn, NULL, "...> ");

    printf("in|line calc - type help, or quit\n");

    for (;;) {
        char *line = inline_readline(edit);
        if (!line) break;

        /* trim leading whitespace for commands */
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        if (!strcmp(p, "quit")) {
            free(line);
            break;
        }
        if (!strcmp(p, "help")) {
            print_help();
            free(line);
            continue;
        }
        if (*p == '\0') {
            free(line);
            continue;
        }

        double result = 0.0;
        const char *err = NULL;
        if (eval(p, &result, &err)) {
            printf("= %.15g\n", result);
        } else {
            printf("error: %s\n", err ? err : "parse error");
        }

        free(line);
    }

    inline_free(edit);
    return 0;
}
