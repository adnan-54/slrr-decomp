// Declaration parser for the SLRR script Java dialect (stage 1 of the .java ->
// TUFA compiler). The whole compilation unit is split into package, imports and
// top-level classes; field initializers and method bodies are kept as token
// ranges (matched by braces only) for later stages.
#pragma once
#include <string>
#include <vector>

#include "script/compiler/lexer.h"

struct TokenRange {
    int begin = 0; // first token index
    int end = 0;   // one past the last token index
};

struct Declarator {
    std::string name;
    int array_dims = 0; // dimensions written after the name: `int x[]`
    bool has_init = false;
    TokenRange init;    // tokens after '=' up to the next ',' or ';'
};

struct FieldDecl {
    std::vector<std::string> modifiers;
    std::string type; // as written, `[]` included (e.g. "int[]")
    std::vector<Declarator> decls;
    int pos = 0; // token index of the declaration, source order
};

struct ParamDecl {
    std::string type; // as written, `[]` included
    std::string name;
};

struct MethodDecl {
    std::vector<std::string> modifiers;
    bool is_ctor = false;
    std::string return_type; // empty for constructors
    std::string name;
    std::vector<ParamDecl> params;
    std::vector<std::string> throws;
    bool has_body = false;
    TokenRange body; // tokens inside the braces, [begin, end)
    int pos = 0;     // token index of the declaration, source order
};

struct ClassDecl {
    std::vector<std::string> modifiers;
    std::string kind; // "class" or "interface"
    std::string name;
    std::string extends; // qualified name, interface list joined with ", "
    std::vector<std::string> implements;
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;
};

struct JavaUnit {
    std::string package;
    std::vector<std::string> imports;
    std::vector<ClassDecl> classes; // top-level classes (nested ones are flattened)
};

// Returns false and sets *err to "<line>: <message>" on a syntax error.
// When tokens is non-null it receives the lexed file (body ranges index it).
bool parse_java(const std::string &src, JavaUnit *out, std::string *err,
                std::vector<Token> *tokens = nullptr);
