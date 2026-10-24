// Statement/expression parser for the SLRR script Java dialect (stage 2 of the
// .java -> TUFA compiler). Parses the token ranges produced by stage 1 (decl.h)
// into the AST of ast.h.
#pragma once
#include <string>
#include <vector>

#include "script/compiler/ast.h"
#include "script/compiler/decl.h"
#include "script/compiler/lexer.h"

// Tokens are the full lexed file; `body` is the range inside the method braces.
// Errors are "<line>: <message>".
bool parse_method_body(const std::vector<Token> &tokens, TokenRange body, BodyAst *out,
                       std::string *err);

// `init` is the initializer token range of one field declarator.
bool parse_field_init(const std::vector<Token> &tokens, TokenRange init, BodyAst *out,
                      std::string *err);
