/*
 * lexer.c — Telos lexer (tokenizer).
 *
 * Tokenizes Telos source code into a TokenArray.
 * Matches the behavior of the Python tokenize() function.
 */

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "telos.h"

/* -----------------------------------------------------------------------
 * Keyword table
 * ----------------------------------------------------------------------- */

static const char *keywords[] = {
    "int", "float", "double", "void", "bool",
    "return", "if", "else", "for", "while", "break", "continue",
    "true", "false",
};

#define NUM_KEYWORDS ((int)(sizeof(keywords) / sizeof(keywords[0])))

static bool is_keyword(const char *word)
{
    for (int i = 0; i < NUM_KEYWORDS; i++) {
        if (strcmp(word, keywords[i]) == 0)
            return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Two-character operator table
 * ----------------------------------------------------------------------- */

static const char *two_char_ops[] = {
    "++", "--", "+=", "-=", "*=", "/=", "%=",
    "<=", ">=", "==", "!=", "&&", "||", "->", "<<", ">>",
};

#define NUM_TWO_CHAR_OPS ((int)(sizeof(two_char_ops) / sizeof(two_char_ops[0])))

static bool is_two_char_op(const char *s)
{
    for (int i = 0; i < NUM_TWO_CHAR_OPS; i++) {
        if (s[0] == two_char_ops[i][0] && s[1] == two_char_ops[i][1])
            return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Helper: create a Token
 * ----------------------------------------------------------------------- */

static Token make_token(TokenKind kind, const char *start, int len, int line, int col)
{
    Token t;
    t.kind = kind;
    t.line = line;
    t.col  = col;

    if (len >= (int)sizeof(t.value))
        len = (int)sizeof(t.value) - 1;
    memcpy(t.value, start, len);
    t.value[len] = '\0';

    return t;
}

/* -----------------------------------------------------------------------
 * Float literal matching
 *
 * Matches the Python regex:
 *   \d+\.\d*([eE][+-]?\d+)?   |   \.\d+([eE][+-]?\d+)?
 *
 * Returns the number of characters consumed, or 0 on no match.
 * ----------------------------------------------------------------------- */

static int match_exponent(const char *s)
{
    int j = 0;
    if (s[j] != 'e' && s[j] != 'E')
        return 0;
    j++;
    if (s[j] == '+' || s[j] == '-')
        j++;
    if (!isdigit((unsigned char)s[j]))
        return 0;
    while (isdigit((unsigned char)s[j]))
        j++;
    return j;
}

static int match_float(const char *s)
{
    int j = 0;

    /* Pattern 1: \d+\.\d*([eE][+-]?\d+)? */
    if (isdigit((unsigned char)s[0])) {
        j = 0;
        while (isdigit((unsigned char)s[j]))
            j++;
        if (s[j] == '.') {
            j++;
            while (isdigit((unsigned char)s[j]))
                j++;
            j += match_exponent(s + j);
            return j;
        }
        /* digits without a dot — not a float */
    }

    /* Pattern 2: \.\d+([eE][+-]?\d+)? */
    if (s[0] == '.') {
        j = 1;
        if (!isdigit((unsigned char)s[j]))
            return 0;
        while (isdigit((unsigned char)s[j]))
            j++;
        j += match_exponent(s + j);
        return j;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * Integer literal matching: \d+
 * ----------------------------------------------------------------------- */

static int match_int(const char *s)
{
    int j = 0;
    while (isdigit((unsigned char)s[j]))
        j++;
    return j;
}

/* -----------------------------------------------------------------------
 * Identifier matching: [a-zA-Z_]\w*
 * ----------------------------------------------------------------------- */

static int match_ident(const char *s)
{
    if (!isalpha((unsigned char)s[0]) && s[0] != '_')
        return 0;
    int j = 1;
    while (isalnum((unsigned char)s[j]) || s[j] == '_')
        j++;
    return j;
}

/* -----------------------------------------------------------------------
 * Character-class helpers
 * ----------------------------------------------------------------------- */

static bool is_op_char(char ch)
{
    return ch && strchr("+-*/%<>!=&|^~", ch) != NULL;
}

static bool is_punct_char(char ch)
{
    return ch && strchr("(){}[];,", ch) != NULL;
}

/* -----------------------------------------------------------------------
 * telos_tokenize
 * ----------------------------------------------------------------------- */

int telos_tokenize(const char *source, TokenArray *out)
{
    da_init(out);

    int i   = 0;
    int len = (int)strlen(source);
    int line = 1;
    int col  = 1;

    while (i < len) {
        int start_line = line;
        int start_col  = col;
        char ch = source[i];

        /* ---- Whitespace ---- */
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            i++; col++;
            continue;
        }
        if (ch == '\n') {
            i++; line++; col = 1;
            continue;
        }

        /* ---- Single-line comment ---- */
        if (i + 1 < len && source[i] == '/' && source[i + 1] == '/') {
            while (i < len && source[i] != '\n') {
                i++; col++;
            }
            continue;
        }

        /* ---- Multi-line comment ---- */
        if (i + 1 < len && source[i] == '/' && source[i + 1] == '*') {
            i += 2; col += 2;
            while (i < len - 1 && !(source[i] == '*' && source[i + 1] == '/')) {
                if (source[i] == '\n') {
                    i++; line++; col = 1;
                } else {
                    i++; col++;
                }
            }
            if (i >= len - 1) {
                fprintf(stderr, "LexError: Unterminated comment at line %d, col %d\n",
                        start_line, start_col);
                da_free(out);
                return -1;
            }
            i += 2; col += 2;
            continue;
        }

        /* ---- Float literal ---- */
        {
            int n = match_float(source + i);
            if (n > 0) {
                Token t = make_token(TOK_FLOAT_LIT, source + i, n, start_line, start_col);
                da_push(out, t);
                i += n; col += n;
                continue;
            }
        }

        /* ---- Integer literal ---- */
        {
            int n = match_int(source + i);
            if (n > 0) {
                Token t = make_token(TOK_INT_LIT, source + i, n, start_line, start_col);
                da_push(out, t);
                i += n; col += n;
                continue;
            }
        }

        /* ---- Identifier / keyword ---- */
        {
            int n = match_ident(source + i);
            if (n > 0) {
                char word[64];
                int wlen = n < (int)sizeof(word) - 1 ? n : (int)sizeof(word) - 1;
                memcpy(word, source + i, wlen);
                word[wlen] = '\0';

                TokenKind kind = is_keyword(word) ? TOK_KEYWORD : TOK_ID;
                Token t = make_token(kind, source + i, n, start_line, start_col);
                da_push(out, t);
                i += n; col += n;
                continue;
            }
        }

        /* ---- Two-character operators ---- */
        if (i + 1 < len && is_two_char_op(source + i)) {
            Token t = make_token(TOK_OP, source + i, 2, start_line, start_col);
            da_push(out, t);
            i += 2; col += 2;
            continue;
        }

        /* ---- Single-character operators ---- */
        if (is_op_char(ch)) {
            Token t = make_token(TOK_OP, source + i, 1, start_line, start_col);
            da_push(out, t);
            i++; col++;
            continue;
        }

        /* ---- Punctuation ---- */
        if (is_punct_char(ch)) {
            Token t = make_token(TOK_PUNCT, source + i, 1, start_line, start_col);
            da_push(out, t);
            i++; col++;
            continue;
        }

        /* ---- Unexpected character ---- */
        fprintf(stderr, "LexError: Unexpected character '%c' at line %d, col %d\n",
                ch, line, col);
        da_free(out);
        return -1;
    }

    /* Append EOF token */
    Token eof = make_token(TOK_EOF, "", 0, line, col);
    da_push(out, eof);
    return 0;
}
