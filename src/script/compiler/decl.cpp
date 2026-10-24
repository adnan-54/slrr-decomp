#include "script/compiler/decl.h"

#include <utility>

#include "script/compiler/lexer.h"

namespace {

bool is_modifier(const std::string &s)
{
    return s == "public" || s == "private" || s == "protected" || s == "static" ||
           s == "final" || s == "abstract" || s == "native" || s == "synchronized" ||
           s == "transient" || s == "volatile" || s == "strictfp";
}

bool is_primitive(const std::string &s)
{
    return s == "boolean" || s == "byte" || s == "char" || s == "short" || s == "int" ||
           s == "long" || s == "float" || s == "double" || s == "void";
}

struct Parser {
    const std::vector<Token> &t;
    size_t pos = 0;
    std::string err;

    explicit Parser(const std::vector<Token> &tokens) : t(tokens) {}

    const Token &cur() const { return t[pos]; }
    const Token &peek(size_t k = 1) const { return t[pos + k < t.size() ? pos + k : t.size() - 1]; }

    bool at(const char *s) const { return cur().kind != TokKind::Eof && cur().text == s; }
    bool eat(const char *s)
    {
        if (!at(s)) return false;
        pos++;
        return true;
    }
    void advance()
    {
        if (cur().kind != TokKind::Eof) pos++;
    }
    bool fail(const std::string &msg)
    {
        if (err.empty()) err = std::to_string(cur().line) + ": " + msg;
        return false;
    }
    bool expect(const char *s)
    {
        return eat(s) || fail(std::string("expected '") + s + "'");
    }

    bool qualified(std::string *out)
    {
        if (cur().kind != TokKind::Ident) return fail("expected identifier");
        *out = cur().text;
        advance();
        while (at(".")) {
            advance();
            if (cur().kind != TokKind::Ident) return fail("expected identifier after '.'");
            *out += "." + cur().text;
            advance();
        }
        return true;
    }

    bool type_name(std::string *out)
    {
        if (cur().kind != TokKind::Ident &&
            !(cur().kind == TokKind::Keyword && is_primitive(cur().text)))
            return fail("expected type");
        *out = cur().text;
        advance();
        while (at(".")) {
            advance();
            if (cur().kind != TokKind::Ident) return fail("expected identifier after '.'");
            *out += "." + cur().text;
            advance();
        }
        while (at("[") && peek().text == "]") {
            *out += "[]";
            advance();
            advance();
        }
        return true;
    }

    bool import_name(std::string *out)
    {
        std::string s;
        while (cur().kind != TokKind::Eof && !at(";")) {
            s += cur().text;
            advance();
        }
        if (s.empty()) return fail("empty import");
        *out = std::move(s);
        return expect(";");
    }

    bool skip_block()
    {
        if (!expect("{")) return false;
        int depth = 1;
        while (depth > 0) {
            if (cur().kind == TokKind::Eof) return fail("unterminated block");
            if (at("{"))
                depth++;
            else if (at("}"))
                depth--;
            advance();
        }
        return true;
    }

    bool scan_initializer(TokenRange *r)
    {
        r->begin = (int)pos;
        int depth = 0;
        while (cur().kind != TokKind::Eof) {
            if (depth == 0 && (at(",") || at(";"))) break;
            if (at("(") || at("[") || at("{")) {
                depth++;
            } else if (at(")") || at("]") || at("}")) {
                if (depth == 0) break;
                depth--;
            }
            advance();
        }
        r->end = (int)pos;
        return true;
    }

    bool method_body(MethodDecl *m)
    {
        if (eat(";")) return true; // native/abstract/interface method
        if (!at("{")) return fail("expected method body or ';'");
        size_t open = pos;
        m->has_body = true;
        int depth = 0;
        do {
            if (cur().kind == TokKind::Eof) return fail("unterminated method body");
            if (at("{"))
                depth++;
            else if (at("}"))
                depth--;
            if (depth == 0) break;
            advance();
        } while (true);
        m->body.begin = (int)open + 1;
        m->body.end = (int)pos;
        advance(); // closing brace
        return true;
    }

    bool parameters(std::vector<ParamDecl> *out)
    {
        if (!expect("(")) return false;
        if (!at(")")) {
            for (;;) {
                if (eat("final")) {
                    // parameter modifier, not part of the type
                }
                ParamDecl p;
                if (!type_name(&p.type)) return false;
                if (cur().kind != TokKind::Ident) return fail("expected parameter name");
                p.name = cur().text;
                advance();
                while (at("[") && peek().text == "]") {
                    p.type += "[]";
                    advance();
                    advance();
                }
                out->push_back(std::move(p));
                if (!eat(",")) break;
            }
        }
        return expect(")");
    }

    bool method_rest(MethodDecl *m)
    {
        if (!parameters(&m->params)) return false;
        if (eat("throws")) {
            for (;;) {
                std::string n;
                if (!qualified(&n)) return false;
                m->throws.push_back(std::move(n));
                if (!eat(",")) break;
            }
        }
        return method_body(m);
    }

    bool declarator_rest(Declarator *d)
    {
        while (at("[") && peek().text == "]") {
            d->array_dims++;
            advance();
            advance();
        }
        if (eat("=")) {
            d->has_init = true;
            if (!scan_initializer(&d->init)) return false;
        }
        return true;
    }

    bool member(JavaUnit *unit, ClassDecl *c)
    {
        std::vector<std::string> mods;
        while (cur().kind == TokKind::Keyword && is_modifier(cur().text)) {
            mods.push_back(cur().text);
            advance();
        }
        if (at("{")) return skip_block(); // initializer block

        if (at("class") || at("interface")) { // nested class, flattened into the unit
            ClassDecl nested;
            if (!class_decl(unit, &nested, std::move(mods))) return false;
            unit->classes.push_back(std::move(nested));
            return true;
        }

        // Constructor: the first identifier is followed by '('.
        if (cur().kind == TokKind::Ident && peek().text == "(") {
            MethodDecl m;
            m.modifiers = std::move(mods);
            m.is_ctor = true;
            m.name = cur().text;
            advance();
            if (!method_rest(&m)) return false;
            c->methods.push_back(std::move(m));
            return true;
        }

        std::string type;
        if (!type_name(&type)) return false;
        if (cur().kind != TokKind::Ident) return fail("expected member name");
        std::string name = cur().text;
        advance();

        if (at("(")) {
            MethodDecl m;
            m.modifiers = std::move(mods);
            m.return_type = std::move(type);
            m.name = std::move(name);
            if (!method_rest(&m)) return false;
            c->methods.push_back(std::move(m));
            return true;
        }

        FieldDecl f;
        f.modifiers = std::move(mods);
        f.type = std::move(type);
        Declarator d;
        d.name = std::move(name);
        if (!declarator_rest(&d)) return false;
        f.decls.push_back(std::move(d));
        while (at(",")) {
            advance();
            if (cur().kind != TokKind::Ident) return fail("expected declarator name");
            Declarator next;
            next.name = cur().text;
            advance();
            if (!declarator_rest(&next)) return false;
            f.decls.push_back(std::move(next));
        }
        if (!expect(";")) return false;
        c->fields.push_back(std::move(f));
        return true;
    }

    bool class_decl(JavaUnit *unit, ClassDecl *c, std::vector<std::string> mods)
    {
        c->modifiers = std::move(mods);
        c->kind = cur().text;
        advance();
        if (cur().kind != TokKind::Ident) return fail("expected class name");
        c->name = cur().text;
        advance();
        if (eat("extends")) {
            std::string n;
            if (!qualified(&n)) return false;
            c->extends = std::move(n);
            while (eat(",")) {
                std::string more;
                if (!qualified(&more)) return false;
                c->extends += ", " + more;
            }
        }
        if (eat("implements")) {
            for (;;) {
                std::string n;
                if (!qualified(&n)) return false;
                c->implements.push_back(std::move(n));
                if (!eat(",")) break;
            }
        }
        if (!expect("{")) return false;
        while (!at("}")) {
            if (cur().kind == TokKind::Eof) return fail("unterminated class body");
            if (eat(";")) continue;
            if (!member(unit, c)) return false;
        }
        advance(); // closing brace
        return true;
    }

    bool unit(JavaUnit *u)
    {
        while (cur().kind != TokKind::Eof) {
            if (eat(";")) continue;
            if (at("package")) {
                advance();
                if (!qualified(&u->package)) return false;
                if (!expect(";")) return false;
                continue;
            }
            if (at("import")) {
                advance();
                std::string n;
                if (!import_name(&n)) return false;
                u->imports.push_back(std::move(n));
                continue;
            }
            std::vector<std::string> mods;
            while (cur().kind == TokKind::Keyword && is_modifier(cur().text)) {
                mods.push_back(cur().text);
                advance();
            }
            if (at("class") || at("interface")) {
                ClassDecl c;
                if (!class_decl(u, &c, std::move(mods))) return false;
                u->classes.push_back(std::move(c));
                continue;
            }
            return fail("expected package, import, class or interface");
        }
        return true;
    }
};

} // namespace

bool parse_java(const std::string &src, JavaUnit *out, std::string *err,
                std::vector<Token> *tokens_out)
{
    std::vector<Token> tokens;
    std::string lex_err;
    if (!lex_java(src, &tokens, &lex_err)) {
        if (err) *err = lex_err;
        return false;
    }
    Parser p(tokens);
    *out = JavaUnit();
    if (!p.unit(out)) {
        if (err) *err = p.err;
        return false;
    }
    if (tokens_out) *tokens_out = std::move(tokens);
    return true;
}
