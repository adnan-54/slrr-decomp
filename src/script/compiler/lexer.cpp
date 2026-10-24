#include "script/compiler/lexer.h"

#include <unordered_set>

namespace {

bool ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
           c >= 0x80; // Latin-1/CP1250 byte: only reached in identifiers or strings
}

bool ident_char(unsigned char c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

bool digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

bool hex_digit(unsigned char c)
{
    return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool keyword(const std::string &s)
{
    static const std::unordered_set<std::string> words = {
        "abstract", "boolean", "break", "byte", "case", "catch", "char", "class",
        "const", "continue", "default", "do", "double", "else", "enum", "extends",
        "false", "final", "finally", "float", "for", "goto", "if", "implements",
        "import", "instanceof", "int", "interface", "long", "native", "new", "null",
        "package", "private", "protected", "public", "return", "short", "static",
        "strictfp", "super", "switch", "synchronized", "this", "throw", "throws",
        "transient", "true", "try", "void", "volatile", "while",
    };
    return words.count(s) != 0;
}

Token make(TokKind kind, const std::string &src, size_t begin, size_t end, int line)
{
    Token t;
    t.kind = kind;
    t.text = src.substr(begin, end - begin);
    t.line = line;
    return t;
}

bool fail(std::string *err, int line, const std::string &msg)
{
    if (err) *err = std::to_string(line) + ": " + msg;
    return false;
}

} // namespace

bool lex_java(const std::string &src, std::vector<Token> *out, std::string *err)
{
    out->clear();
    size_t i = 0;
    const size_t n = src.size();
    int line = 1;

    if (n >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF)
        i = 3; // UTF-8 BOM

    while (i < n) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n') {
            line++;
            i++;
            continue;
        }
        if (space(c)) {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            int start_line = line;
            i += 2;
            while (i < n && !(src[i] == '*' && i + 1 < n && src[i + 1] == '/')) {
                if (src[i] == '\n') line++;
                i++;
            }
            if (i >= n) return fail(err, start_line, "unterminated comment");
            i += 2;
            continue;
        }

        if (ident_start(c)) {
            size_t start = i++;
            while (i < n && ident_char((unsigned char)src[i])) i++;
            size_t end = i;
            // Look ahead for `<name>(.<name>)*:0x<hex>r` without consuming.
            size_t j = end;
            for (;;) {
                if (j < n && src[j] == '.' && j + 1 < n &&
                    ident_start((unsigned char)src[j + 1])) {
                    j += 2;
                    while (j < n && ident_char((unsigned char)src[j])) j++;
                } else {
                    break;
                }
            }
            if (j < n && src[j] == ':') {
                size_t k = j + 1;
                if (k + 2 < n && src[k] == '0' && (src[k + 1] == 'x' || src[k + 1] == 'X')) {
                    size_t h = k + 2;
                    while (h < n && hex_digit((unsigned char)src[h])) h++;
                    if (h > k + 2 && h < n && src[h] == 'r') {
                        Token t = make(TokKind::Resource, src, start, h + 1, line);
                        out->push_back(t);
                        i = h + 1;
                        continue;
                    }
                }
            }
            Token t = make(keyword(src.substr(start, end - start)) ? TokKind::Keyword
                                                                   : TokKind::Ident,
                           src, start, end, line);
            out->push_back(t);
            continue;
        }

        if (digit(c) || (c == '.' && i + 1 < n && digit((unsigned char)src[i + 1]))) {
            size_t start = i;
            bool is_float = false;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                while (i < n && hex_digit((unsigned char)src[i])) i++;
                if (i < n && (src[i] == 'l' || src[i] == 'L')) i++;
            } else {
                if (c == '.') {
                    is_float = true;
                    i++;
                    while (i < n && digit((unsigned char)src[i])) i++;
                } else {
                    while (i < n && digit((unsigned char)src[i])) i++;
                    if (i < n && src[i] == '.') {
                        is_float = true;
                        i++;
                        while (i < n && digit((unsigned char)src[i])) i++;
                    }
                }
                if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                    size_t e = i + 1;
                    if (e < n && (src[e] == '+' || src[e] == '-')) e++;
                    if (e < n && digit((unsigned char)src[e])) {
                        is_float = true;
                        i = e;
                        while (i < n && digit((unsigned char)src[i])) i++;
                    }
                }
                if (i < n && (src[i] == 'f' || src[i] == 'F' || src[i] == 'd' || src[i] == 'D')) {
                    is_float = true;
                    i++;
                } else if (!is_float && i < n && (src[i] == 'l' || src[i] == 'L')) {
                    i++;
                }
            }
            out->push_back(make(is_float ? TokKind::Float : TokKind::Int, src, start, i, line));
            continue;
        }

        if (c == '"' || c == '\'') {
            int start_line = line;
            size_t start = i;
            unsigned char quote = c;
            i++;
            bool closed = false;
            while (i < n) {
                if (src[i] == '\\') {
                    if (i + 1 >= n) {
                        i++;
                        break;
                    }
                    if (src[i + 1] == '\n') line++;
                    i += 2;
                    continue;
                }
                if ((unsigned char)src[i] == quote) {
                    i++;
                    closed = true;
                    break;
                }
                if (src[i] == '\n') line++;
                i++;
            }
            if (!closed)
                return fail(err, start_line,
                            quote == '"' ? "unterminated string" : "unterminated char literal");
            out->push_back(make(quote == '"' ? TokKind::String : TokKind::Char, src, start, i,
                                start_line));
            continue;
        }

        Token t;
        t.kind = TokKind::Punct;
        t.text = std::string(1, (char)c);
        t.line = line;
        out->push_back(t);
        i++;
    }

    Token eof;
    eof.kind = TokKind::Eof;
    eof.line = line;
    out->push_back(eof);
    return true;
}
