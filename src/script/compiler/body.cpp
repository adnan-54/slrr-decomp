#include "script/compiler/body.h"

namespace {

bool primitive_kw(const std::string &s)
{
    return s == "boolean" || s == "byte" || s == "char" || s == "short" || s == "int" ||
           s == "long" || s == "float" || s == "double";
}

bool uppercase(const std::string &s)
{
    return !s.empty() && s[0] >= 'A' && s[0] <= 'Z';
}

// Recursive-descent parser over one token range. Composite operators are made
// of consecutive one-character Punct tokens (the lexer does not merge them).
struct BodyParser {
    const std::vector<Token> &t;
    size_t pos;
    size_t end;
    std::string err;

    BodyParser(const std::vector<Token> &tokens, TokenRange r)
        : t(tokens), pos((size_t)r.begin), end((size_t)r.end)
    {
    }

    bool at_end() const { return pos >= end; }
    const Token &cur() const { return t[pos < t.size() ? pos : t.size() - 1]; }
    const Token &peek(size_t k = 1) const
    {
        size_t i = pos + k;
        return t[i < t.size() ? i : t.size() - 1];
    }

    bool fail(const std::string &msg)
    {
        if (err.empty()) err = std::to_string(cur().line) + ": " + msg;
        return false;
    }

    bool at(const char *s) const { return cur().kind != TokKind::Eof && cur().text == s; }
    bool at_kw(const char *s) const
    {
        return cur().kind == TokKind::Keyword && cur().text == s;
    }
    bool at_ident() const { return cur().kind == TokKind::Ident; }
    bool eat(const char *s)
    {
        if (!at(s)) return false;
        pos++;
        return true;
    }
    bool expect(const char *s)
    {
        return eat(s) || fail(std::string("expected '") + s + "'");
    }

    // The lexer emits each Java operator as one merged Punct token.
    bool at_op(const char *s) const
    {
        return pos < end && t[pos].kind == TokKind::Punct && t[pos].text == s;
    }

    std::unique_ptr<Expr> mk(ExprKind kind, int line, std::string text = std::string())
    {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->line = line;
        e->text = std::move(text);
        return e;
    }

    // ---- types ---------------------------------------------------------

    bool parse_type(std::string *out)
    {
        if (cur().kind == TokKind::Keyword && primitive_kw(cur().text)) {
            *out = cur().text;
            pos++;
        } else if (at_ident()) {
            *out = cur().text;
            pos++;
            while (at(".") && peek().kind == TokKind::Ident) {
                *out += "." + peek().text;
                pos += 2;
            }
        } else {
            return fail("expected type");
        }
        while (at("[") && peek().text == "]") {
            *out += "[]";
            pos += 2;
        }
        return true;
    }

    // Non-consuming: is a local variable declaration at pos? Types are
    // primitive keywords or (qualified) identifiers; no generics in the dialect.
    bool is_local_decl() const
    {
        size_t i = pos;
        if (i < end && t[i].kind == TokKind::Keyword && t[i].text == "final") i++;
        if (i >= end) return false;
        if (t[i].kind == TokKind::Keyword && primitive_kw(t[i].text)) {
            i++;
        } else if (t[i].kind == TokKind::Ident) {
            i++;
            while (i + 1 < end && t[i].kind == TokKind::Punct && t[i].text == "." &&
                   t[i + 1].kind == TokKind::Ident)
                i += 2;
        } else {
            return false;
        }
        while (i + 1 < end && t[i].text == "[" && t[i + 1].text == "]") i += 2;
        if (i >= end || t[i].kind != TokKind::Ident) return false;
        i++; // the declared name
        while (i + 1 < end && t[i].text == "[" && t[i + 1].text == "]") i += 2;
        if (i >= end) return false;
        return t[i].text == "=" || t[i].text == "," || t[i].text == ";";
    }

    // ---- expressions ---------------------------------------------------

    bool at_assign() const
    {
        static const char *ops[] = {"=",  "+=", "-=",  "*=",  "/=",  "%=",
                                    "&=", "|=", "^=",  "<<=", ">>=", ">>>="};
        for (const char *s : ops)
            if (at_op(s)) return true;
        return false;
    }

    bool assign_op(ExprKind *kind) const
    {
        struct Op {
            const char *s;
            ExprKind k;
        };
        static const Op ops[] = {
            {">>>=", ExprKind::UshrAssign}, {"<<=", ExprKind::ShlAssign},
            {">>=", ExprKind::ShrAssign},   {"+=", ExprKind::AddAssign},
            {"-=", ExprKind::SubAssign},    {"*=", ExprKind::MulAssign},
            {"/=", ExprKind::DivAssign},    {"%=", ExprKind::ModAssign},
            {"&=", ExprKind::BitAndAssign}, {"|=", ExprKind::BitOrAssign},
            {"^=", ExprKind::BitXorAssign}, {"=", ExprKind::Assign},
        };
        for (const Op &op : ops) {
            if (at_op(op.s)) {
                *kind = op.k;
                return true;
            }
        }
        return false;
    }

    int bin_op(ExprKind *kind) const
    {
        if (at_assign()) return 0;
        struct Op {
            const char *s;
            int prec;
            ExprKind k;
        };
        static const Op ops[] = {
            {"||", 1, ExprKind::LogOr},   {"&&", 2, ExprKind::LogAnd},
            {"|", 3, ExprKind::BitOr},    {"^", 4, ExprKind::BitXor},
            {"&", 5, ExprKind::BitAnd},   {"==", 6, ExprKind::Eq},
            {"!=", 6, ExprKind::Ne},      {"<=", 7, ExprKind::Le},
            {">=", 7, ExprKind::Ge},      {"<<", 8, ExprKind::Shl},
            {">>>", 8, ExprKind::Ushr},   {">>", 8, ExprKind::Shr},
            {"<", 7, ExprKind::Lt},       {">", 7, ExprKind::Gt},
            {"+", 9, ExprKind::Add},      {"-", 9, ExprKind::Sub},
            {"*", 10, ExprKind::Mul},     {"/", 10, ExprKind::Div},
            {"%", 10, ExprKind::Mod},
        };
        for (const Op &op : ops) {
            if (at_op(op.s)) {
                *kind = op.k;
                return op.prec;
            }
        }
        return 0;
    }

    bool starts_operand(size_t i) const
    {
        if (i >= end) return false;
        const Token &n = t[i];
        switch (n.kind) {
        case TokKind::Ident:
        case TokKind::Int:
        case TokKind::Float:
        case TokKind::String:
        case TokKind::Char:
        case TokKind::Resource:
            return true;
        case TokKind::Keyword:
            return n.text == "true" || n.text == "false" || n.text == "null" ||
                   n.text == "this" || n.text == "super" || n.text == "new";
        case TokKind::Punct:
            return n.text == "(" || n.text == "!" || n.text == "~" || n.text == "+" ||
                   n.text == "-" || n.text == "++" || n.text == "--";
        default:
            return false;
        }
    }

    // Casts need type resolution this parser does not have. INVENTED: the
    // dialect capitalises class names, so only a capitalised (possibly
    // qualified) type is taken as a cast; `(name) - x` stays an expression.
    bool looks_like_cast() const
    {
        size_t i = pos + 1;
        if (i >= end) return false;
        if (t[i].kind == TokKind::Keyword && primitive_kw(t[i].text)) {
            i++;
        } else if (t[i].kind == TokKind::Ident) {
            std::string last = t[i].text;
            i++;
            while (i + 1 < end && t[i].kind == TokKind::Punct && t[i].text == "." &&
                   t[i + 1].kind == TokKind::Ident) {
                last = t[i + 1].text;
                i += 2;
            }
            if (!uppercase(last)) return false;
        } else {
            return false;
        }
        while (i + 1 < end && t[i].text == "[" && t[i + 1].text == "]") i += 2;
        if (i >= end || t[i].kind != TokKind::Punct || t[i].text != ")") return false;
        return starts_operand(i + 1);
    }

    bool parse_args(std::vector<std::unique_ptr<Expr>> *out)
    {
        if (!expect("(")) return false;
        if (!at(")")) {
            for (;;) {
                auto a = parse_expr();
                if (!a) return false;
                out->push_back(std::move(a));
                if (!eat(",")) break;
            }
        }
        return eat(")") || fail("expected ')'");
    }

    std::unique_ptr<Expr> parse_array_init()
    {
        int line = cur().line;
        if (!expect("{")) return nullptr;
        auto e = mk(ExprKind::ArrayInit, line);
        while (!at("}")) {
            if (at_end()) {
                fail("unterminated array initializer");
                return nullptr;
            }
            auto v = parse_expr();
            if (!v) return nullptr;
            e->kids.push_back(std::move(v));
            if (!eat(",")) break;
        }
        if (!expect("}")) return nullptr;
        return e;
    }

    std::unique_ptr<Expr> parse_new()
    {
        int line = cur().line;
        pos++; // new
        std::string type;
        if (cur().kind == TokKind::Keyword && primitive_kw(cur().text)) {
            type = cur().text;
            pos++;
        } else if (at_ident()) {
            type = cur().text;
            pos++;
            while (at(".") && peek().kind == TokKind::Ident) {
                type += "." + peek().text;
                pos += 2;
            }
        } else {
            fail("expected type after 'new'");
            return nullptr;
        }
        if (at("[")) {
            auto e = mk(ExprKind::NewArray, line);
            int dims = 0;
            while (at("[")) {
                pos++;
                dims++;
                if (eat("]")) continue; // empty dimension
                auto d = parse_expr();
                if (!d) return nullptr;
                e->kids.push_back(std::move(d));
                if (!expect("]")) return nullptr;
            }
            for (int i = 0; i < dims; i++) type += "[]";
            e->text = std::move(type);
            if (at("{")) {
                e->flag = true; // last kid is the initializer
                auto init = parse_array_init();
                if (!init) return nullptr;
                e->kids.push_back(std::move(init));
            }
            return e;
        }
        if (!at("(")) {
            fail("expected '(' or '[' after new type");
            return nullptr;
        }
        auto e = mk(ExprKind::New, line, std::move(type));
        if (!parse_args(&e->kids)) return nullptr;
        return e;
    }

    std::unique_ptr<Expr> parse_primary()
    {
        if (at_end()) {
            fail("unexpected end of body");
            return nullptr;
        }
        const Token &tok = cur();
        switch (tok.kind) {
        case TokKind::Int:
            pos++;
            return mk(ExprKind::IntLit, tok.line, tok.text);
        case TokKind::Float:
            pos++;
            return mk(ExprKind::FloatLit, tok.line, tok.text);
        case TokKind::String:
            pos++;
            return mk(ExprKind::StringLit, tok.line, tok.text);
        case TokKind::Char:
            pos++;
            return mk(ExprKind::CharLit, tok.line, tok.text);
        case TokKind::Resource:
            pos++;
            return mk(ExprKind::ResourceLit, tok.line, tok.text);
        case TokKind::Ident:
            pos++;
            return mk(ExprKind::Name, tok.line, tok.text);
        case TokKind::Keyword:
            if (tok.text == "true" || tok.text == "false") {
                auto e = mk(ExprKind::BoolLit, tok.line, tok.text);
                e->flag = tok.text == "true";
                pos++;
                return e;
            }
            if (tok.text == "null") {
                pos++;
                return mk(ExprKind::Null, tok.line);
            }
            if (tok.text == "this") {
                pos++;
                return mk(ExprKind::This, tok.line);
            }
            if (tok.text == "super") {
                pos++;
                return mk(ExprKind::Super, tok.line);
            }
            if (tok.text == "new") return parse_new();
            fail("unexpected keyword '" + tok.text + "'");
            return nullptr;
        case TokKind::Punct:
            if (tok.text == "(") {
                pos++;
                auto e = parse_expr();
                if (!e) return nullptr;
                if (!expect(")")) return nullptr;
                return e;
            }
            if (tok.text == "{") return parse_array_init();
            fail("unexpected '" + tok.text + "'");
            return nullptr;
        default:
            fail("unexpected token");
            return nullptr;
        }
    }

    std::unique_ptr<Expr> parse_postfix()
    {
        auto e = parse_primary();
        if (!e) return nullptr;
        for (;;) {
            if (at_end()) break;
            if (at(".")) {
                int line = cur().line;
                pos++;
                if (at_kw("class")) {
                    pos++;
                    auto c = mk(ExprKind::ClassLit, line);
                    c->kids.push_back(std::move(e));
                    e = std::move(c);
                    continue;
                }
                if (!at_ident()) {
                    fail("expected member name after '.'");
                    return nullptr;
                }
                std::string name = cur().text;
                pos++;
                if (at("(")) {
                    auto call = mk(ExprKind::Call, line, std::move(name));
                    call->kids.push_back(std::move(e)); // receiver
                    if (!parse_args(&call->kids)) return nullptr;
                    e = std::move(call);
                } else {
                    auto m = mk(ExprKind::Member, line, std::move(name));
                    m->kids.push_back(std::move(e));
                    e = std::move(m);
                }
            } else if (at("(") && e->kind == ExprKind::Name) {
                auto call = mk(ExprKind::Call, e->line, e->text);
                if (!parse_args(&call->kids)) return nullptr;
                e = std::move(call);
            } else if (at("[")) {
                int line = cur().line;
                pos++;
                auto idx = parse_expr();
                if (!idx) return nullptr;
                if (!expect("]")) return nullptr;
                auto ix = mk(ExprKind::Index, line);
                ix->kids.push_back(std::move(e));
                ix->kids.push_back(std::move(idx));
                e = std::move(ix);
            } else if (at_op("++")) {
                pos++;
                auto p = mk(ExprKind::PostInc, e->line);
                p->kids.push_back(std::move(e));
                e = std::move(p);
            } else if (at_op("--")) {
                pos++;
                auto p = mk(ExprKind::PostDec, e->line);
                p->kids.push_back(std::move(e));
                e = std::move(p);
            } else {
                break;
            }
        }
        return e;
    }

    std::unique_ptr<Expr> parse_unary()
    {
        if (at_op("++") || at_op("--")) {
            bool inc = at_op("++");
            int line = cur().line;
            pos++;
            auto v = parse_unary();
            if (!v) return nullptr;
            auto e = mk(inc ? ExprKind::PreInc : ExprKind::PreDec, line);
            e->kids.push_back(std::move(v));
            return e;
        }
        if (at("!") || at("~") || at("+") || at("-")) {
            int line = cur().line;
            ExprKind kind = at("!")   ? ExprKind::Not
                            : at("~") ? ExprKind::BitNot
                            : at("+") ? ExprKind::UnaryPlus
                                      : ExprKind::Neg;
            pos++;
            auto v = parse_unary();
            if (!v) return nullptr;
            auto e = mk(kind, line);
            e->kids.push_back(std::move(v));
            return e;
        }
        if (at("(") && looks_like_cast()) {
            int line = cur().line;
            pos++;
            std::string type;
            if (!parse_type(&type)) return nullptr;
            if (!expect(")")) return nullptr;
            auto v = parse_unary();
            if (!v) return nullptr;
            auto e = mk(ExprKind::Cast, line, std::move(type));
            e->kids.push_back(std::move(v));
            return e;
        }
        return parse_postfix();
    }

    std::unique_ptr<Expr> parse_binary(int min_prec)
    {
        auto lhs = parse_unary();
        if (!lhs) return nullptr;
        for (;;) {
            if (at_kw("instanceof")) {
                if (7 < min_prec) break;
                int line = cur().line;
                pos++;
                std::string type;
                if (!parse_type(&type)) return nullptr;
                auto e = mk(ExprKind::InstanceOf, line, std::move(type));
                e->kids.push_back(std::move(lhs));
                lhs = std::move(e);
                continue;
            }
            ExprKind kind = ExprKind::Null;
            int prec = bin_op(&kind);
            if (prec == 0 || prec < min_prec) break;
            int line = cur().line;
            pos++;
            auto rhs = parse_binary(prec + 1);
            if (!rhs) return nullptr;
            auto e = mk(kind, line);
            e->kids.push_back(std::move(lhs));
            e->kids.push_back(std::move(rhs));
            lhs = std::move(e);
        }
        return lhs;
    }

    std::unique_ptr<Expr> parse_ternary()
    {
        auto cond = parse_binary(1);
        if (!cond) return nullptr;
        if (!at("?")) return cond;
        int line = cur().line;
        pos++;
        auto then_e = parse_assignment(); // middle may be an assignment
        if (!then_e) return nullptr;
        if (!expect(":")) return nullptr;
        auto else_e = parse_ternary();
        if (!else_e) return nullptr;
        auto e = mk(ExprKind::Ternary, line);
        e->kids.push_back(std::move(cond));
        e->kids.push_back(std::move(then_e));
        e->kids.push_back(std::move(else_e));
        return e;
    }

    std::unique_ptr<Expr> parse_assignment()
    {
        auto lhs = parse_ternary();
        if (!lhs) return nullptr;
        ExprKind kind = ExprKind::Null;
        if (!assign_op(&kind)) return lhs;
        int line = cur().line;
        pos++;
        auto rhs = parse_assignment();
        if (!rhs) return nullptr;
        auto e = mk(kind, line);
        e->kids.push_back(std::move(lhs));
        e->kids.push_back(std::move(rhs));
        return e;
    }

    std::unique_ptr<Expr> parse_expr() { return parse_assignment(); }

    // ---- statements ----------------------------------------------------

    std::unique_ptr<Stmt> parse_block()
    {
        int line = cur().line;
        if (!expect("{")) return nullptr;
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Block;
        s->line = line;
        while (!at("}")) {
            if (at_end()) {
                fail("unterminated block");
                return nullptr;
            }
            auto st = parse_stmt();
            if (!st) return nullptr;
            s->stmts.push_back(std::move(st));
        }
        s->end_line = cur().line;
        pos++; // closing brace
        return s;
    }

    std::unique_ptr<Stmt> parse_local_decl(bool semicolon)
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::LocalDecl;
        s->line = cur().line;
        if (at_kw("final")) pos++;
        if (!parse_type(&s->type)) return nullptr;
        for (;;) {
            if (!at_ident()) {
                fail("expected variable name");
                return nullptr;
            }
            DeclAst d;
            d.line = cur().line;
            d.name = cur().text;
            pos++;
            while (at("[") && peek().text == "]") {
                d.dims++;
                pos += 2;
            }
            if (eat("=")) {
                d.init = parse_expr();
                if (!d.init) return nullptr;
            }
            s->decls.push_back(std::move(d));
            if (!eat(",")) break;
        }
        if (semicolon && !expect(";")) return nullptr;
        return s;
    }

    std::unique_ptr<Stmt> parse_if()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::If;
        s->line = cur().line;
        pos++;
        if (!expect("(")) return nullptr;
        s->expr = parse_expr();
        if (!s->expr) return nullptr;
        if (!expect(")")) return nullptr;
        s->then_stmt = parse_stmt();
        if (!s->then_stmt) return nullptr;
        if (eat("else")) {
            s->else_stmt = parse_stmt();
            if (!s->else_stmt) return nullptr;
        }
        return s;
    }

    std::unique_ptr<Stmt> parse_while()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::While;
        s->line = cur().line;
        pos++;
        if (!expect("(")) return nullptr;
        s->expr = parse_expr();
        if (!s->expr) return nullptr;
        if (!expect(")")) return nullptr;
        s->body = parse_stmt();
        if (!s->body) return nullptr;
        return s;
    }

    std::unique_ptr<Stmt> parse_do()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::DoWhile;
        s->line = cur().line;
        pos++;
        s->body = parse_stmt();
        if (!s->body) return nullptr;
        if (!expect("while")) return nullptr;
        if (!expect("(")) return nullptr;
        s->expr = parse_expr();
        if (!s->expr) return nullptr;
        if (!expect(")")) return nullptr;
        if (!expect(";")) return nullptr;
        return s;
    }

    bool parse_expr_list(std::vector<std::unique_ptr<Expr>> *out)
    {
        for (;;) {
            auto e = parse_expr();
            if (!e) return false;
            out->push_back(std::move(e));
            if (!eat(",")) break;
        }
        return true;
    }

    std::unique_ptr<Stmt> parse_for()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::For;
        s->line = cur().line;
        pos++;
        if (!expect("(")) return nullptr;
        if (!at(";")) {
            if (is_local_decl()) {
                s->init_stmt = parse_local_decl(false);
                if (!s->init_stmt) return nullptr;
            } else if (!parse_expr_list(&s->init_exprs)) {
                return nullptr;
            }
        }
        if (!expect(";")) return nullptr;
        if (!at(";")) {
            s->expr = parse_expr();
            if (!s->expr) return nullptr;
            s->has_cond = true;
        }
        if (!expect(";")) return nullptr;
        if (!at(")")) {
            if (!parse_expr_list(&s->update_exprs)) return nullptr;
        }
        if (!expect(")")) return nullptr;
        s->body = parse_stmt();
        if (!s->body) return nullptr;
        return s;
    }

    std::unique_ptr<Stmt> parse_switch()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Switch;
        s->line = cur().line;
        pos++;
        if (!expect("(")) return nullptr;
        s->expr = parse_expr();
        if (!s->expr) return nullptr;
        if (!expect(")")) return nullptr;
        if (!expect("{")) return nullptr;
        SwitchCase *active = nullptr;
        while (!at("}")) {
            if (at_end()) {
                fail("unterminated switch");
                return nullptr;
            }
            if (at_kw("case") || at_kw("default")) {
                SwitchCase c;
                c.line = cur().line;
                c.is_default = at_kw("default");
                pos++;
                if (!c.is_default) {
                    c.label = parse_expr();
                    if (!c.label) return nullptr;
                }
                if (!expect(":")) return nullptr;
                s->cases.push_back(std::move(c));
                active = &s->cases.back();
                continue;
            }
            if (!active) {
                fail("statement before the first case label");
                return nullptr;
            }
            auto st = parse_stmt();
            if (!st) return nullptr;
            active->stmts.push_back(std::move(st));
        }
        s->end_line = cur().line;
        pos++;
        return s;
    }

    std::unique_ptr<Stmt> parse_jump()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = at_kw("break") ? StmtKind::Break : StmtKind::Continue;
        s->line = cur().line;
        pos++;
        if (at_ident()) {
            s->label = cur().text;
            pos++;
        }
        if (!expect(";")) return nullptr;
        return s;
    }

    std::unique_ptr<Stmt> parse_return()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Return;
        s->line = cur().line;
        pos++;
        if (!eat(";")) {
            s->expr = parse_expr();
            if (!s->expr) return nullptr;
            if (!expect(";")) return nullptr;
        }
        return s;
    }

    std::unique_ptr<Stmt> parse_ctor_call()
    {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::CtorCall;
        s->line = cur().line;
        s->is_super = at_kw("super");
        pos++;
        if (!parse_args(&s->args)) return nullptr;
        if (!expect(";")) return nullptr;
        return s;
    }

    std::unique_ptr<Stmt> parse_stmt()
    {
        if (at_end()) {
            fail("unexpected end of body");
            return nullptr;
        }
        if (at("{")) return parse_block();
        int line = cur().line;
        if (eat(";")) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Empty;
            s->line = line;
            return s;
        }
        if (at_kw("if")) return parse_if();
        if (at_kw("while")) return parse_while();
        if (at_kw("do")) return parse_do();
        if (at_kw("for")) return parse_for();
        if (at_kw("switch")) return parse_switch();
        if (at_kw("break") || at_kw("continue")) return parse_jump();
        if (at_kw("return")) return parse_return();
        if ((at_kw("this") || at_kw("super")) && peek().text == "(") return parse_ctor_call();
        if (is_local_decl()) return parse_local_decl(true);
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Expr;
        s->line = line;
        s->expr = parse_expr();
        if (!s->expr) return nullptr;
        if (!expect(";")) return nullptr;
        return s;
    }
};

bool report(const BodyParser &p, std::string *err)
{
    if (err) *err = p.err.empty() ? std::to_string(p.cur().line) + ": parse error" : p.err;
    return false;
}

} // namespace

bool parse_method_body(const std::vector<Token> &tokens, TokenRange body, BodyAst *out,
                       std::string *err)
{
    *out = BodyAst();
    BodyParser p(tokens, body);
    if ((size_t)body.end < tokens.size()) out->close_line = tokens[(size_t)body.end].line;
    while (!p.at_end()) {
        auto st = p.parse_stmt();
        if (!st) return report(p, err);
        out->stmts.push_back(std::move(st));
    }
    return true;
}

bool parse_field_init(const std::vector<Token> &tokens, TokenRange init, BodyAst *out,
                      std::string *err)
{
    *out = BodyAst();
    BodyParser p(tokens, init);
    if (p.at_end()) {
        if (err) *err = std::to_string(p.cur().line) + ": empty initializer";
        return false;
    }
    auto e = p.parse_expr();
    if (!e) return report(p, err);
    if (!p.at_end()) {
        if (err)
            *err = std::to_string(p.cur().line) + ": unexpected '" + p.cur().text +
                   "' after initializer";
        return false;
    }
    out->init = std::move(e);
    return true;
}
