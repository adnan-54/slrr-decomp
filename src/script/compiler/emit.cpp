// Class skeleton writer. See emit.h and FORMAT.md "SLRR TUFA".
#include "script/compiler/emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "script/compiler/ast.h"
#include "script/compiler/body.h"

namespace {

// Shipped .class header words (FORMAT.md "SLRR TUFA": version 4; 0x00024F9D is
// the stamp of the stock corpus, notes/03 says the loader also accepts 0x11AE1).
const uint32_t kVersion = 4;
const uint32_t kStamp = 0x00024F9Du;

bool has_mod(const std::vector<std::string> &mods, const char *m)
{
    for (const std::string &s : mods)
        if (s == m) return true;
    return false;
}

// FILD/MTHD flag bits observed in the shipped pairs: public 0x01, private 0x04,
// static 0x08, final 0x20, native 0x40. Other modifiers do not occur in the
// pair corpus.
uint32_t modifier_flags(const std::vector<std::string> &mods)
{
    uint32_t f = 0;
    if (has_mod(mods, "public")) f |= 0x01;
    if (has_mod(mods, "private")) f |= 0x04;
    if (has_mod(mods, "static")) f |= 0x08;
    if (has_mod(mods, "final")) f |= 0x20;
    if (has_mod(mods, "native")) f |= 0x40;
    return f;
}

struct TypeSpelling {
    std::string base;
    int dims = 0;
};

TypeSpelling split_type(const std::string &s)
{
    TypeSpelling t;
    t.base = s;
    while (t.base.size() >= 2 && t.base.compare(t.base.size() - 2, 2, "[]") == 0) {
        t.base.resize(t.base.size() - 2);
        t.dims++;
    }
    return t;
}

const char *primitive_desc(const std::string &s)
{
    if (s == "void") return "V";
    if (s == "boolean") return "Z";
    if (s == "byte") return "B";
    if (s == "char") return "C";
    if (s == "short") return "S";
    if (s == "int") return "I";
    if (s == "long") return "J";
    if (s == "float") return "F";
    if (s == "double") return "D";
    return nullptr;
}

bool primitive_type(const std::string &s) { return primitive_desc(s) != nullptr; }

bool object_desc(const std::string &d)
{
    return d.size() >= 2 && d[0] == 'L' && d[d.size() - 1] == ';';
}

std::string desc_class(const std::string &d)
{
    return object_desc(d) ? d.substr(1, d.size() - 2) : std::string();
}

// Parameter descriptors of a method signature, e.g. "(IF)V" -> {"I","F"}.
std::vector<std::string> sig_params(const std::string &sig)
{
    std::vector<std::string> out;
    size_t i = sig.find('(');
    if (i == std::string::npos) return out;
    i++;
    while (i < sig.size() && sig[i] != ')') {
        size_t start = i;
        while (i < sig.size() && sig[i] == '[') i++;
        if (i >= sig.size()) break;
        if (sig[i] == 'L') {
            size_t end = sig.find(';', i);
            if (end == std::string::npos) break;
            i = end + 1;
        } else {
            i++;
        }
        out.push_back(sig.substr(start, i - start));
    }
    return out;
}

std::string sig_return(const std::string &sig)
{
    size_t i = sig.find(')');
    return i == std::string::npos ? std::string() : sig.substr(i + 1);
}

int count_sig_params(const std::string &sig) { return (int)sig_params(sig).size(); }

std::string unquote(const std::string &raw)
{
    std::string out;
    if (raw.size() < 2) return raw;
    for (size_t i = 1; i + 1 < raw.size(); i++) {
        char c = raw[i];
        if (c != '\\' || i + 2 >= raw.size()) {
            out += c;
            continue;
        }
        char e = raw[++i];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '\\': out += '\\'; break;
        case '\'': out += '\''; break;
        case '"': out += '"'; break;
        default: out += e; break; // INVENTED: unknown escapes kept verbatim
        }
    }
    return out;
}

struct ResourceLit {
    std::string name; // "parts\engines\Foo.rpk"
    uint32_t id = 0;
};

// `parts.engines.Foo:0x0000009Br` -> dots become path separators + .rpk.
ResourceLit parse_resource(const std::string &raw)
{
    ResourceLit r;
    size_t colon = raw.rfind(':');
    if (colon == std::string::npos) {
        r.name = raw;
        return r;
    }
    std::string path = raw.substr(0, colon);
    std::string val = raw.substr(colon + 1);
    if (!val.empty() && (val.back() == 'r' || val.back() == 'R')) val.pop_back();
    r.id = (uint32_t)strtoul(val.c_str(), nullptr, 16);
    for (char &c : path)
        if (c == '.') c = '\\';
    r.name = path + ".rpk";
    return r;
}

class Emitter {
public:
    Emitter(const JavaUnit &unit, const std::vector<Token> &tokens, const ClassIndex &index,
            const ClassDecl &cls)
        : unit_(unit), tok_(tokens), index_(index), cls_(cls)
    {
    }

    bool run(ClassUnit *out, std::string *err);

private:
    const JavaUnit &unit_;
    const std::vector<Token> &tok_;
    const ClassIndex &index_;
    const ClassDecl &cls_;

    std::string fqn_, super_;
    std::vector<std::string> ifaces_;
    bool is_interface_ = false;

    ClassUnit u_;
    ClassSym self_;
    std::map<std::string, int> intern_;
    std::vector<std::vector<std::pair<std::string, std::string>>> scopes_;
    int locals_ = 0;

    // ---- constants -----------------------------------------------------

    int add_str(const std::string &s)
    {
        std::string key = "s" + s;
        auto it = intern_.find(key);
        if (it != intern_.end()) return it->second;
        ConstEntry e;
        e.type = CONST_STRING;
        e.str = s;
        u_.consts.push_back(e);
        int idx = (int)u_.consts.size() - 1;
        intern_[key] = idx;
        return idx;
    }

    int add_class(const std::string &name)
    {
        std::string key = "c" + name;
        auto it = intern_.find(key);
        if (it != intern_.end()) return it->second;
        ConstEntry e;
        e.type = CONST_CLASS;
        e.a = add_str(name);
        u_.consts.push_back(e);
        int idx = (int)u_.consts.size() - 1;
        intern_[key] = idx;
        return idx;
    }

    int add_nt(const std::string &name, const std::string &desc)
    {
        std::string key = "n" + name + "\x1f" + desc;
        auto it = intern_.find(key);
        if (it != intern_.end()) return it->second;
        ConstEntry e;
        e.type = CONST_NAME_TYPE;
        e.a = add_str(name);
        e.b = add_str(desc);
        u_.consts.push_back(e);
        int idx = (int)u_.consts.size() - 1;
        intern_[key] = idx;
        return idx;
    }

    int add_member(const std::string &cls, const std::string &name, const std::string &desc)
    {
        if (cls.empty()) return -1;
        std::string key = "m" + cls + "\x1f" + name + "\x1f" + desc;
        auto it = intern_.find(key);
        if (it != intern_.end()) return it->second;
        int c = add_class(cls);
        int nt = add_nt(name, desc);
        ConstEntry e;
        e.type = CONST_MEMBER;
        e.a = c;
        e.b = nt;
        u_.consts.push_back(e);
        int idx = (int)u_.consts.size() - 1;
        intern_[key] = idx;
        return idx;
    }

    int add_resource(const std::string &name, uint32_t id)
    {
        std::string key = "r" + name + std::to_string(id);
        auto it = intern_.find(key);
        if (it != intern_.end()) return it->second;
        ConstEntry e;
        e.type = CONST_RESOURCE;
        e.a = add_str(name);
        e.b = (int32_t)id;
        u_.consts.push_back(e);
        int idx = (int)u_.consts.size() - 1;
        intern_[key] = idx;
        return idx;
    }

    // ---- type names ----------------------------------------------------

    std::string resolve_class(const std::string &name) const
    {
        if (name.empty()) return name;
        if (name.find('.') != std::string::npos) return name;
        std::vector<std::string> cand;
        if (name == cls_.name) cand.push_back(fqn_);
        for (const ClassDecl &c : unit_.classes) {
            if (c.name == name && &c != &cls_)
                cand.push_back(unit_.package.empty() ? name : unit_.package + "." + name);
        }
        if (!unit_.package.empty()) cand.push_back(unit_.package + "." + name);
        for (const std::string &im : unit_.imports) {
            if (im.size() > 2 && im.compare(im.size() - 2, 2, ".*") == 0) {
                cand.push_back(im.substr(0, im.size() - 2) + "." + name);
            } else if (im.size() > name.size() + 1 &&
                       im[im.size() - name.size() - 1] == '.' &&
                       im.compare(im.size() - name.size(), name.size(), name) == 0) {
                cand.push_back(im);
            }
        }
        cand.push_back("java.lang." + name);
        for (const std::string &c : cand)
            if (index_.find(c)) return c;
        // INVENTED: unresolved name -> first structural guess, so descriptors
        // stay well-formed; the CONS comparison reports the difference.
        return cand.empty() ? name : cand.front();
    }

    std::string type_desc(const std::string &spelling) const
    {
        TypeSpelling t = split_type(spelling);
        const char *p = primitive_desc(t.base);
        std::string base = p ? std::string(p) : "L" + resolve_class(t.base) + ";";
        return std::string((size_t)t.dims, '[') + base;
    }

    // CONST_CLASS payload naming: dotted FQN, arrays as "["*n + base
    // (observed: "java.lang.String", "[F", "[java.lang.String").
    std::string type_ref_name(const std::string &spelling) const
    {
        TypeSpelling t = split_type(spelling);
        const char *p = primitive_desc(t.base);
        std::string base = p ? std::string(p) : resolve_class(t.base);
        return std::string((size_t)t.dims, '[') + base;
    }

    // ---- symbols -------------------------------------------------------

    void build_self_sym();
    const ClassSym *chain_class(const std::string &name) const
    {
        if (name == fqn_) return &self_;
        return index_.find(name);
    }
    const FieldSym *find_field(const std::string &cls, const std::string &name,
                               std::string *owner) const;
    const MethodSym *find_method(const std::string &cls, const std::string &name,
                                 const std::vector<std::string> &args,
                                 std::string *owner) const;

    bool local_type(const std::string &name, std::string *desc) const
    {
        for (size_t i = scopes_.size(); i-- > 0;)
            for (auto it = scopes_[i].rbegin(); it != scopes_[i].rend(); ++it)
                if (it->first == name) {
                    *desc = it->second;
                    return true;
                }
        return false;
    }
    void declare_local(const std::string &name, const std::string &desc)
    {
        if (scopes_.empty()) scopes_.push_back({});
        scopes_.back().push_back({name, desc});
        locals_++;
    }

    bool class_receiver(const Expr &e, std::string *cls) const;
    std::string infer(const Expr &e) const;

    // ---- constant collection ------------------------------------------

    void expr(const Expr &e);
    void emit_member(const Expr &e);
    void emit_call(const Expr &e);
    void scan_stmt(const Stmt &s);
    void scan_stmts(const std::vector<std::unique_ptr<Stmt>> &stmts);
};

void Emitter::build_self_sym()
{
    self_.name = fqn_;
    self_.super = super_;
    for (const FieldDecl &f : cls_.fields) {
        bool st = has_mod(f.modifiers, "static");
        for (const Declarator &d : f.decls) {
            FieldSym fs;
            fs.name = d.name;
            fs.desc = type_desc(f.type + std::string((size_t)d.array_dims * 2, '['));
            fs.is_static = st;
            self_.fields.push_back(std::move(fs));
        }
    }
    for (const MethodDecl &m : cls_.methods) {
        MethodSym ms;
        ms.name = m.is_ctor ? "<init>" : m.name;
        ms.sig = "(";
        for (const ParamDecl &p : m.params) ms.sig += type_desc(p.type);
        ms.sig += ")";
        if (!m.is_ctor) ms.sig += type_desc(m.return_type);
        ms.is_static = has_mod(m.modifiers, "static");
        self_.methods.push_back(std::move(ms));
    }
}

const FieldSym *Emitter::find_field(const std::string &cls, const std::string &name,
                                    std::string *owner) const
{
    std::string cur = cls;
    for (int guard = 0; !cur.empty() && guard < 64; guard++) {
        const ClassSym *s = chain_class(cur);
        if (!s) break;
        for (const FieldSym &f : s->fields)
            if (f.name == name) {
                if (owner) *owner = cur;
                return &f;
            }
        cur = s->super;
    }
    return nullptr;
}

const MethodSym *Emitter::find_method(const std::string &cls, const std::string &name,
                                      const std::vector<std::string> &args,
                                      std::string *owner) const
{
    const MethodSym *fallback = nullptr;
    std::string fallback_owner;
    std::string cur = cls;
    for (int guard = 0; !cur.empty() && guard < 64; guard++) {
        const ClassSym *s = chain_class(cur);
        if (!s) break;
        // The exe scans a class from the LAST declared method to the first.
        for (auto it = s->methods.rbegin(); it != s->methods.rend(); ++it) {
            if (it->name != name) continue;
            if ((size_t)count_sig_params(it->sig) != args.size()) continue;
            std::vector<std::string> ps = sig_params(it->sig);
            bool exact = true;
            for (size_t i = 0; i < ps.size(); i++)
                if (args[i].empty() || ps[i] != args[i]) exact = false;
            if (exact) {
                if (owner) *owner = cur;
                return &*it;
            }
            if (!fallback) {
                fallback = &*it;
                fallback_owner = cur;
            }
        }
        cur = s->super;
    }
    if (fallback && owner) *owner = fallback_owner;
    return fallback;
}

bool Emitter::class_receiver(const Expr &e, std::string *cls) const
{
    if (e.kind != ExprKind::Name) return false;
    std::string ld;
    if (local_type(e.text, &ld)) return false;
    std::string owner;
    if (find_field(fqn_, e.text, &owner)) return false;
    std::string c = resolve_class(e.text);
    if (c.empty()) return false;
    if (index_.find(c) || c == fqn_ || (!e.text.empty() && e.text[0] >= 'A' && e.text[0] <= 'Z')) {
        *cls = c;
        return true;
    }
    return false;
}

std::string Emitter::infer(const Expr &e) const
{
    switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::BoolLit:
    case ExprKind::ResourceLit:
    case ExprKind::InstanceOf:
    case ExprKind::LogOr:
    case ExprKind::LogAnd:
    case ExprKind::Eq:
    case ExprKind::Ne:
    case ExprKind::Ge:
    case ExprKind::Le:
    case ExprKind::Gt:
    case ExprKind::Lt:
    case ExprKind::Not:
        return "I";
    case ExprKind::FloatLit:
        return "F";
    case ExprKind::CharLit:
        return "C";
    case ExprKind::StringLit:
        return "Ljava.lang.String;";
    case ExprKind::This:
        return "L" + fqn_ + ";";
    case ExprKind::Super:
        return super_.empty() ? std::string() : "L" + super_ + ";";
    case ExprKind::New:
        return "L" + resolve_class(e.text) + ";";
    case ExprKind::NewArray:
    case ExprKind::Cast:
        return type_desc(e.text);
    case ExprKind::Name: {
        std::string ld;
        if (local_type(e.text, &ld)) return ld;
        std::string owner;
        if (const FieldSym *f = find_field(fqn_, e.text, &owner)) return f->desc;
        return std::string();
    }
    case ExprKind::Member: {
        if (e.kids.empty()) return std::string();
        std::string cls;
        if (class_receiver(*e.kids[0], &cls)) {
            std::string owner;
            if (const FieldSym *f = find_field(cls, e.text, &owner)) return f->desc;
            return std::string();
        }
        std::string rd = infer(*e.kids[0]);
        if (!rd.empty() && rd[0] == '[') return e.text == "length" ? "I" : std::string();
        std::string c = desc_class(rd);
        if (c.empty()) return std::string();
        std::string owner;
        if (const FieldSym *f = find_field(c, e.text, &owner)) return f->desc;
        return std::string();
    }
    case ExprKind::Call: {
        std::string cls = fqn_;
        size_t first_arg = 0;
        if (e.flag && !e.kids.empty()) {
            if (!class_receiver(*e.kids[0], &cls)) cls = desc_class(infer(*e.kids[0]));
            first_arg = 1;
        }
        if (cls.empty()) return std::string();
        std::vector<std::string> args;
        for (size_t i = first_arg; i < e.kids.size(); i++) args.push_back(infer(*e.kids[i]));
        std::string owner;
        if (const MethodSym *m = find_method(cls, e.text, args, &owner))
            return sig_return(m->sig);
        return std::string();
    }
    case ExprKind::Index: {
        if (e.kids.empty()) return std::string();
        std::string rd = infer(*e.kids[0]);
        return rd.size() > 1 && rd[0] == '[' ? rd.substr(1) : std::string();
    }
    case ExprKind::Assign:
    case ExprKind::MulAssign:
    case ExprKind::DivAssign:
    case ExprKind::ModAssign:
    case ExprKind::AddAssign:
    case ExprKind::SubAssign:
    case ExprKind::ShlAssign:
    case ExprKind::ShrAssign:
    case ExprKind::UshrAssign:
    case ExprKind::BitAndAssign:
    case ExprKind::BitXorAssign:
    case ExprKind::BitOrAssign:
    case ExprKind::PostInc:
    case ExprKind::PostDec:
    case ExprKind::PreInc:
    case ExprKind::PreDec:
    case ExprKind::UnaryPlus:
    case ExprKind::Neg:
    case ExprKind::BitNot:
        return e.kids.empty() ? std::string() : infer(*e.kids[0]);
    case ExprKind::Add:
    case ExprKind::Sub:
    case ExprKind::Mul:
    case ExprKind::Div:
    case ExprKind::Mod:
    case ExprKind::Shl:
    case ExprKind::Shr:
    case ExprKind::Ushr:
    case ExprKind::BitAnd:
    case ExprKind::BitOr:
    case ExprKind::BitXor: {
        if (e.kids.size() < 2) return std::string();
        std::string a = infer(*e.kids[0]), b = infer(*e.kids[1]);
        return a == "F" || b == "F" ? "F" : "I";
    }
    case ExprKind::Ternary:
        return e.kids.size() == 3 ? infer(*e.kids[1]) : std::string();
    case ExprKind::ClassLit:
        return "Ljava.lang.Class;";
    case ExprKind::Null:
    case ExprKind::ArrayInit:
    case ExprKind::CtorChain:
    case ExprKind::Const:
        return std::string();
    }
    return std::string();
}

void Emitter::emit_member(const Expr &e)
{
    if (!e.kids.empty()) {
        const Expr &recv = *e.kids[0];
        std::string cls;
        if (class_receiver(recv, &cls)) {
            std::string owner;
            if (const FieldSym *f = find_field(cls, e.text, &owner))
                add_member(cls, e.text, f->desc);
        } else {
            std::string rd = infer(recv);
            if (!rd.empty() && rd[0] == '[') {
                if (e.text == "length") add_member(rd, "length", "I");
            } else {
                std::string c = desc_class(rd);
                if (!c.empty()) {
                    std::string owner;
                    if (const FieldSym *f = find_field(c, e.text, &owner))
                        add_member(c, e.text, f->desc);
                }
            }
        }
    }
    for (const auto &k : e.kids) expr(*k);
}

void Emitter::emit_call(const Expr &e)
{
    std::string cls = fqn_;
    size_t first_arg = 0;
    if (e.flag && !e.kids.empty()) {
        const Expr &recv = *e.kids[0];
        first_arg = 1;
        if (recv.kind == ExprKind::This) {
            cls = fqn_;
        } else if (recv.kind == ExprKind::Super) {
            cls = super_;
        } else if (class_receiver(recv, &cls)) {
            // static receiver
        } else {
            cls = desc_class(infer(recv));
        }
    }
    std::vector<std::string> args;
    for (size_t i = first_arg; i < e.kids.size(); i++) args.push_back(infer(*e.kids[i]));
    if (!cls.empty()) {
        std::string owner;
        if (const MethodSym *m = find_method(cls, e.text, args, &owner))
            add_member(cls, e.text, m->sig);
    }
    for (const auto &k : e.kids) expr(*k);
}

void Emitter::expr(const Expr &e)
{
    switch (e.kind) {
    case ExprKind::StringLit:
        add_str(unquote(e.text));
        break;
    case ExprKind::ResourceLit: {
        ResourceLit r = parse_resource(e.text);
        add_resource(r.name, r.id);
        break;
    }
    case ExprKind::Name: {
        std::string ld;
        if (local_type(e.text, &ld)) break;
        std::string owner;
        if (const FieldSym *f = find_field(fqn_, e.text, &owner)) {
            add_member(f->is_static ? owner : fqn_, e.text, f->desc);
            break;
        }
        std::string cls;
        if (class_receiver(e, &cls)) add_class(cls);
        break;
    }
    case ExprKind::Member:
        emit_member(e);
        break;
    case ExprKind::Call:
        emit_call(e);
        break;
    case ExprKind::New:
    case ExprKind::NewArray:
        add_class(type_ref_name(e.text));
        for (const auto &k : e.kids) expr(*k);
        break;
    case ExprKind::Cast: {
        TypeSpelling t = split_type(e.text);
        if (t.dims > 0 || !primitive_type(t.base)) add_class(type_ref_name(e.text));
        for (const auto &k : e.kids) expr(*k);
        break;
    }
    case ExprKind::InstanceOf:
        add_class(type_ref_name(e.text));
        for (const auto &k : e.kids) expr(*k);
        break;
    case ExprKind::ClassLit:
        if (!e.kids.empty()) {
            std::string cls;
            if (class_receiver(*e.kids[0], &cls)) add_class(cls);
        }
        for (const auto &k : e.kids) expr(*k);
        break;
    default:
        for (const auto &k : e.kids) expr(*k);
        break;
    }
}

void Emitter::scan_stmt(const Stmt &s)
{
    switch (s.kind) {
    case StmtKind::Block:
        scopes_.push_back({});
        for (const auto &child : s.stmts) scan_stmt(*child);
        scopes_.pop_back();
        break;
    case StmtKind::LocalDecl:
        for (const DeclAst &d : s.decls) {
            if (d.init) expr(*d.init);
            declare_local(d.name, type_desc(s.type + std::string((size_t)d.dims * 2, '[')));
        }
        break;
    case StmtKind::If:
        expr(*s.expr);
        scan_stmt(*s.then_stmt);
        if (s.else_stmt) scan_stmt(*s.else_stmt);
        break;
    case StmtKind::While:
    case StmtKind::DoWhile:
        if (s.expr) expr(*s.expr);
        scan_stmt(*s.body);
        break;
    case StmtKind::For:
        scopes_.push_back({});
        if (s.init_stmt) scan_stmt(*s.init_stmt);
        for (const auto &e : s.init_exprs) expr(*e);
        if (s.expr) expr(*s.expr);
        for (const auto &e : s.update_exprs) expr(*e);
        scan_stmt(*s.body);
        scopes_.pop_back();
        break;
    case StmtKind::Switch:
        scopes_.push_back({});
        expr(*s.expr);
        for (const SwitchCase &c : s.cases) {
            if (c.label) expr(*c.label);
            for (const auto &child : c.stmts) scan_stmt(*child);
        }
        scopes_.pop_back();
        break;
    case StmtKind::Expr:
    case StmtKind::Return:
        if (s.expr) expr(*s.expr);
        break;
    case StmtKind::CtorCall:
        for (const auto &e : s.args) expr(*e);
        break;
    case StmtKind::Empty:
    case StmtKind::Break:
    case StmtKind::Continue:
        break;
    }
}

void Emitter::scan_stmts(const std::vector<std::unique_ptr<Stmt>> &stmts)
{
    for (const auto &s : stmts) scan_stmt(*s);
}

bool Emitter::run(ClassUnit *out, std::string *err)
{
    is_interface_ = cls_.kind == "interface";
    fqn_ = unit_.package.empty() ? cls_.name : unit_.package + "." + cls_.name;

    if (is_interface_) {
        std::vector<std::string> ext;
        size_t p = 0;
        while (p < cls_.extends.size()) {
            size_t comma = cls_.extends.find(',', p);
            ext.push_back(cls_.extends.substr(
                p, comma == std::string::npos ? std::string::npos : comma - p));
            if (comma == std::string::npos) break;
            p = comma + 2; // parser joins with ", "
        }
        if (!ext.empty()) {
            super_ = resolve_class(ext[0]); // INVENTED: first superinterface
            for (size_t i = 1; i < ext.size(); i++) ifaces_.push_back(resolve_class(ext[i]));
        }
    } else {
        super_ = cls_.extends.empty() ? "java.lang.Object" : resolve_class(cls_.extends);
        for (const std::string &n : cls_.implements) ifaces_.push_back(resolve_class(n));
    }
    build_self_sym();

    u_.version = kVersion;
    u_.stamp = kStamp;
    u_.clss_unknown = -1;
    u_.interface_flag = is_interface_ ? 1 : 0;
    u_.self_class = add_class(fqn_);
    u_.super_class = super_.empty() ? -1 : add_class(super_);
    for (const std::string &n : ifaces_) u_.interfaces.push_back(add_class(n));

    // Pass A: parse every initializer/body once, collecting its constants.
    std::vector<std::vector<bool>> field_has_init(cls_.fields.size());
    for (size_t fi = 0; fi < cls_.fields.size(); fi++) {
        field_has_init[fi].resize(cls_.fields[fi].decls.size(), false);
        for (size_t di = 0; di < cls_.fields[fi].decls.size(); di++) {
            const Declarator &d = cls_.fields[fi].decls[di];
            if (!d.has_init) continue;
            BodyAst body;
            std::string perr;
            if (!parse_field_init(tok_, d.init, &body, &perr)) {
                if (err) *err = cls_.name + "." + d.name + ": " + perr;
                return false;
            }
            scopes_.assign(1, {});
            expr(*body.init);
            field_has_init[fi][di] = true;
        }
    }
    std::vector<int> method_locals(cls_.methods.size(), 0);
    for (size_t mi = 0; mi < cls_.methods.size(); mi++) {
        const MethodDecl &m = cls_.methods[mi];
        if (!m.has_body) continue;
        BodyAst body;
        std::string perr;
        if (!parse_method_body(tok_, m.body, &body, &perr)) {
            if (err) *err = cls_.name + "." + m.name + ": " + perr;
            return false;
        }
        scopes_.assign(1, {});
        locals_ = 0;
        if (!has_mod(m.modifiers, "static")) locals_++; // `this`
        for (const ParamDecl &p : m.params) declare_local(p.name, type_desc(p.type));
        scan_stmts(body.stmts);
        method_locals[mi] = locals_;
        scopes_.clear();
    }

    // Pass B: FILD static section then instance, declaration order. Bodies are
    // allocated in this order (field initializers first).
    for (int section = 0; section < 2; section++) {
        for (size_t fi = 0; fi < cls_.fields.size(); fi++) {
            const FieldDecl &f = cls_.fields[fi];
            bool st = has_mod(f.modifiers, "static");
            if (st != (section == 0)) continue;
            for (size_t di = 0; di < f.decls.size(); di++) {
                const Declarator &d = f.decls[di];
                FieldInfo info;
                info.is_static = st;
                info.flags = modifier_flags(f.modifiers);
                info.name = add_str(d.name);
                info.type = add_str(type_desc(f.type + std::string((size_t)d.array_dims * 2, '[')));
                info.init = -1;
                if (field_has_init[fi][di]) {
                    info.init = (int)u_.bodies.size();
                    u_.bodies.push_back(Body());
                }
                u_.fields.push_back(info);
            }
        }
    }

    // Pass C: MTHD static section then instance. Every method gets a body,
    // empty when the source has none (native/abstract).
    for (int section = 0; section < 2; section++) {
        for (size_t mi = 0; mi < cls_.methods.size(); mi++) {
            const MethodDecl &m = cls_.methods[mi];
            bool st = has_mod(m.modifiers, "static");
            if (st != (section == 0)) continue;
            MethodInfo info;
            info.is_static = st;
            info.flags = modifier_flags(m.modifiers);
            info.name = add_str(m.is_ctor ? "<init>" : m.name);
            std::string sig = "(";
            for (const ParamDecl &p : m.params) sig += type_desc(p.type);
            sig += ")";
            if (!m.is_ctor) sig += type_desc(m.return_type);
            info.sig = add_str(sig);
            info.body = (int)u_.bodies.size();
            u_.bodies.push_back(Body());
            info.locals = m.has_body ? method_locals[mi] : 0;
            u_.methods.push_back(info);
        }
    }

    u_.has_fields = !u_.fields.empty();
    u_.has_methods = !u_.methods.empty();
    u_.has_tree = true;
    *out = std::move(u_);
    return true;
}

void put_u32(std::vector<uint8_t> *out, uint32_t v)
{
    out->push_back((uint8_t)(v & 0xff));
    out->push_back((uint8_t)((v >> 8) & 0xff));
    out->push_back((uint8_t)((v >> 16) & 0xff));
    out->push_back((uint8_t)((v >> 24) & 0xff));
}

void put_i32(std::vector<uint8_t> *out, int32_t v) { put_u32(out, (uint32_t)v); }

void put_chunk(std::vector<uint8_t> *out, const char *tag, const std::vector<uint8_t> &data)
{
    out->insert(out->end(), tag, tag + 4);
    put_u32(out, (uint32_t)data.size());
    out->insert(out->end(), data.begin(), data.end());
}

void write_cons(const ClassUnit &u, std::vector<uint8_t> *out)
{
    put_u32(out, (uint32_t)u.consts.size());
    for (const ConstEntry &e : u.consts) {
        put_u32(out, e.type);
        if (e.type == CONST_STRING) {
            put_u32(out, (uint32_t)e.str.size());
            out->insert(out->end(), e.str.begin(), e.str.end());
            out->push_back(0);
        } else if (e.type == CONST_CLASS) {
            put_i32(out, e.a);
        } else {
            put_i32(out, e.a);
            put_i32(out, e.b);
        }
    }
}

void write_fild(const ClassUnit &u, std::vector<uint8_t> *out)
{
    for (int section = 0; section < 2; section++) {
        uint32_t count = 0;
        for (const FieldInfo &f : u.fields)
            if (f.is_static == (section == 0)) count++;
        put_u32(out, count);
        for (const FieldInfo &f : u.fields) {
            if (f.is_static != (section == 0)) continue;
            put_u32(out, f.flags);
            put_i32(out, f.name);
            put_i32(out, f.type);
            put_i32(out, f.init);
        }
    }
}

void write_mthd(const ClassUnit &u, std::vector<uint8_t> *out)
{
    for (int section = 0; section < 2; section++) {
        uint32_t count = 0;
        for (const MethodInfo &m : u.methods)
            if (m.is_static == (section == 0)) count++;
        put_u32(out, count);
        for (const MethodInfo &m : u.methods) {
            if (m.is_static != (section == 0)) continue;
            put_u32(out, m.flags);
            put_i32(out, m.name);
            put_i32(out, m.sig);
            put_i32(out, m.body);
            put_i32(out, m.locals);
        }
    }
}

void write_clss(const ClassUnit &u, std::vector<uint8_t> *out)
{
    put_i32(out, u.clss_unknown);
    put_i32(out, u.interface_flag);
    put_i32(out, u.self_class);
    put_i32(out, u.super_class);
    put_u32(out, (uint32_t)u.interfaces.size());
    for (int32_t i : u.interfaces) put_i32(out, i);
}

void write_tree(const ClassUnit &u, std::vector<uint8_t> *out)
{
    put_u32(out, (uint32_t)u.bodies.size());
    for (size_t i = 0; i < u.bodies.size(); i++) put_u32(out, 0); // TREE is stage 4
}

} // namespace

void ClassIndex::add(const ClassUnit &u)
{
    ClassSym s;
    s.name = u.class_name(u.self_class);
    if (s.name.empty()) return;
    s.super = u.super_class >= 0 ? u.class_name(u.super_class) : std::string();
    for (const FieldInfo &f : u.fields) {
        FieldSym fs;
        fs.name = u.str(f.name) ? u.str(f.name) : "";
        fs.desc = u.str(f.type) ? u.str(f.type) : "";
        fs.is_static = f.is_static;
        if (!fs.name.empty()) s.fields.push_back(std::move(fs));
    }
    for (const MethodInfo &m : u.methods) {
        MethodSym ms;
        ms.name = u.str(m.name) ? u.str(m.name) : "";
        ms.sig = u.str(m.sig) ? u.str(m.sig) : "";
        ms.is_static = m.is_static;
        if (!ms.name.empty()) s.methods.push_back(std::move(ms));
    }
    auto it = by_name_.find(s.name);
    if (it != by_name_.end()) {
        classes_[it->second] = std::move(s);
        return;
    }
    by_name_[s.name] = classes_.size();
    classes_.push_back(std::move(s));
}

const ClassSym *ClassIndex::find(const std::string &name) const
{
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &classes_[it->second];
}

bool compile_unit(const JavaUnit &src, const std::vector<Token> &tokens,
                  const ClassIndex &index, ClassFile *out, std::string *err)
{
    out->units.clear();
    for (const ClassDecl &cls : src.classes) {
        Emitter em(src, tokens, index, cls);
        ClassUnit unit;
        if (!em.run(&unit, err)) return false;
        out->units.push_back(std::move(unit));
    }
    return true;
}

bool classfile_write(const ClassFile &file, std::vector<uint8_t> *out)
{
    out->clear();
    for (const ClassUnit &u : file.units) {
        out->insert(out->end(), "TUFA", "TUFA" + 4);
        put_u32(out, u.version);
        put_u32(out, u.stamp);
        std::vector<uint8_t> cons, fild, mthd, clss, tree;
        write_cons(u, &cons);
        put_chunk(out, "CONS", cons);
        if (u.has_fields) {
            write_fild(u, &fild);
            put_chunk(out, "FILD", fild);
        }
        if (u.has_methods) {
            write_mthd(u, &mthd);
            put_chunk(out, "MTHD", mthd);
        }
        write_clss(u, &clss);
        put_chunk(out, "CLSS", clss);
        write_tree(u, &tree);
        put_chunk(out, "TREE", tree);
    }
    return true;
}
