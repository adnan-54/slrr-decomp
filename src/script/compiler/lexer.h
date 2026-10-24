// Lexer for the SLRR script Java dialect (stage 1 of the .java -> TUFA compiler).
// Source is raw bytes: comments may hold CP1250/Latin-1 text and are never
// decoded. Resource constants such as `cars.racers.Whisper:0x00000158r` are a
// single token.
#pragma once
#include <string>
#include <vector>

enum class TokKind {
    Ident,
    Keyword,
    Int,
    Float,
    String,
    Char,
    Resource,
    Punct,
    Eof,
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text; // source spelling (quotes included for string/char)
    int line = 0;     // 1-based line of the first byte
};

// Returns false and sets *err to "<line>: <message>" on a lexical error.
bool lex_java(const std::string &src, std::vector<Token> *out, std::string *err);
