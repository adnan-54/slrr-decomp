// TUFA .class parser. See classfile.h and notes/03-script-vm.md.
#include "script/classfile.h"

#include <stdio.h>
#include <string.h>

#include "platform/fs.h"

namespace {

// Little-endian cursor with bounds checks; err is set once, at the failure site.
struct Reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
    std::string *err;

    bool fail(const std::string &what)
    {
        if (err && err->empty())
            *err = what + " at offset " + std::to_string(pos);
        return false;
    }
    bool need(size_t n)
    {
        if (n > size - pos)
            return fail("truncated");
        return true;
    }
    size_t remaining() const { return size - pos; }
    bool u8(uint8_t *v)
    {
        if (!need(1)) return false;
        *v = data[pos++];
        return true;
    }
    bool u16(uint16_t *v)
    {
        if (!need(2)) return false;
        *v = (uint16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return true;
    }
    bool u32(uint32_t *v)
    {
        if (!need(4)) return false;
        *v = (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) |
             ((uint32_t)data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        pos += 4;
        return true;
    }
    bool i32(int32_t *v)
    {
        uint32_t t;
        if (!u32(&t)) return false;
        *v = (int32_t)t;
        return true;
    }
    bool bytes(size_t n, const uint8_t **p)
    {
        if (!need(n)) return false;
        *p = data + pos;
        pos += n;
        return true;
    }
};

// Opcode -> payload size, baked from the decompiler's corpus-verified table
// (bin/tools/slrr-script-decompiler/src/SlrrDecompiler/Format/OpcodeTable.cs).
// One wrong entry mis-aligns every body using that opcode, so unknown ops error.
int opcode_size(uint8_t op)
{
    switch (op) {
    case 0x01: case 0x02: case 0x03: case 0x06: case 0x07: case 0x08:
    case 0x09: case 0x0a: case 0x0b: case 0x0d: case 0x0e: case 0x11:
    case 0x12: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19:
    case 0x1a: case 0x1b: case 0x1c: case 0x25: case 0x26: case 0x27:
        return 4;
    case 0x04: case 0x05: case 0x0c: case 0x10: case 0x18: case 0x20:
    case 0x21: case 0x22: case 0x23: case 0x24: case 0x28: case 0x29:
    case 0x2a: case 0x2b: case 0x2c: case 0x2d:
        return 0;
    default:
        return -1;
    }
}

std::string hex32(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

bool parse_const_pool(Reader &r, ClassUnit &u)
{
    uint32_t count;
    if (!r.u32(&count)) return false;
    // Smallest SLRR entry is 8 bytes (classref), except a string = 9; this bound
    // also catches a misparsed count before allocating.
    if ((uint64_t)count * 8 > r.remaining() && count != 0)
        return r.fail("CONS count " + std::to_string(count) + " impossible for chunk");
    u.consts.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        ConstEntry e;
        if (!r.u32(&e.type)) return false;
        switch (e.type) {
        case CONST_STRING: {
            uint32_t len;
            const uint8_t *p;
            if (!r.u32(&len)) return false;
            if (!r.bytes(len, &p)) return false;
            e.str.assign((const char *)p, len);
            uint8_t nul;
            if (!r.u8(&nul)) return false;
            if (nul != 0)
                return r.fail("CONS string #" + std::to_string(i) + " missing NUL");
            break;
        }
        case CONST_RESOURCE:
            if (!r.i32(&e.a) || !r.i32(&e.b)) return false;
            break;
        case CONST_CLASS:
            if (!r.i32(&e.a)) return false;
            break;
        case CONST_MEMBER:
        case CONST_NAME_TYPE:
            if (!r.i32(&e.a) || !r.i32(&e.b)) return false;
            break;
        default:
            return r.fail("CONS #" + std::to_string(i) + " unknown type " +
                          std::to_string(e.type));
        }
        u.consts.push_back(std::move(e));
    }
    return true;
}

bool parse_fields(Reader &r, ClassUnit &u)
{
    for (int section = 0; section < 2; section++) {
        uint32_t count;
        if (!r.u32(&count)) return false;
        if ((uint64_t)count * 16 > r.remaining())
            return r.fail("FILD count " + std::to_string(count) + " impossible for chunk");
        for (uint32_t i = 0; i < count; i++) {
            FieldInfo f;
            f.is_static = section == 0;
            if (!r.u32(&f.flags) || !r.i32(&f.name) || !r.i32(&f.type) ||
                !r.i32(&f.init))
                return false;
            u.fields.push_back(f);
        }
    }
    return true;
}

bool parse_methods(Reader &r, ClassUnit &u)
{
    for (int section = 0; section < 2; section++) {
        uint32_t count;
        if (!r.u32(&count)) return false;
        if ((uint64_t)count * 20 > r.remaining())
            return r.fail("MTHD count " + std::to_string(count) + " impossible for chunk");
        for (uint32_t i = 0; i < count; i++) {
            MethodInfo m;
            m.is_static = section == 0;
            if (!r.u32(&m.flags) || !r.i32(&m.name) || !r.i32(&m.sig) ||
                !r.i32(&m.body) || !r.i32(&m.locals))
                return false;
            u.methods.push_back(m);
        }
    }
    return true;
}

bool parse_class(Reader &r, ClassUnit &u)
{
    if (!r.i32(&u.clss_unknown) || !r.i32(&u.interface_flag) ||
        !r.i32(&u.self_class) || !r.i32(&u.super_class))
        return false;
    uint32_t count;
    if (!r.u32(&count)) return false;
    if ((uint64_t)count * 4 > r.remaining())
        return r.fail("CLSS interface count impossible for chunk");
    for (uint32_t i = 0; i < count; i++) {
        int32_t idx;
        if (!r.i32(&idx)) return false;
        u.interfaces.push_back(idx);
    }
    return true;
}

bool parse_tree(Reader &r, ClassUnit &u)
{
    uint32_t body_count;
    if (!r.u32(&body_count)) return false;
    if ((uint64_t)body_count * 4 > r.remaining())
        return r.fail("TREE body count " + std::to_string(body_count) + " impossible");
    u.bodies.reserve(body_count);
    for (uint32_t b = 0; b < body_count; b++) {
        uint32_t instr_count;
        if (!r.u32(&instr_count)) return false;
        if ((uint64_t)instr_count * 3 > r.remaining())
            return r.fail("TREE body " + std::to_string(b) + " instr count impossible");
        Body body;
        body.instrs.reserve(instr_count);
        for (uint32_t i = 0; i < instr_count; i++) {
            Instr ins;
            ins.file_offset = (uint32_t)r.pos;
            if (!r.u8(&ins.op) || !r.u16(&ins.line)) return false;
            int size = opcode_size(ins.op);
            if (size < 0)
                return r.fail("TREE body " + std::to_string(b) + " instr " +
                              std::to_string(i) + ": unknown opcode " + hex32(ins.op));
            ins.has_operand = size > 0;
            if (size > 0 && !r.u32(&ins.operand)) return false;
            body.byte_length += 3 + (uint32_t)size;
            body.instrs.push_back(ins);
        }
        u.bodies.push_back(std::move(body));
    }
    u.has_tree = true;
    return true;
}

const ConstEntry *get_entry(const ClassUnit &u, int idx)
{
    if (idx < 0 || (size_t)idx >= u.consts.size())
        return nullptr;
    return &u.consts[(size_t)idx];
}

bool check_string_ref(const ClassUnit &u, int idx, const char *what, std::string *err)
{
    const ConstEntry *e = get_entry(u, idx);
    if (!e || e->type != CONST_STRING) {
        *err = std::string(what) + " #" + std::to_string(idx) + " is not a string const";
        return false;
    }
    return true;
}

bool check_class_ref(const ClassUnit &u, int idx, const char *what, std::string *err)
{
    const ConstEntry *e = get_entry(u, idx);
    if (!e || e->type != CONST_CLASS) {
        *err = std::string(what) + " #" + std::to_string(idx) + " is not a class const";
        return false;
    }
    return check_string_ref(u, e->a, what, err);
}

// Cross-check every pool reference and body index once the whole unit is read.
bool validate_unit(const ClassUnit &u, std::string *err)
{
    for (size_t i = 0; i < u.consts.size(); i++) {
        const ConstEntry &e = u.consts[i];
        std::string at = "constant #" + std::to_string(i) + ": ";
        std::string e2;
        bool ok = true;
        switch (e.type) {
        case CONST_STRING:
            break;
        case CONST_RESOURCE:
            ok = check_string_ref(u, e.a, "resource name", &e2);
            break;
        case CONST_CLASS:
            ok = check_string_ref(u, e.a, "class name", &e2);
            break;
        case CONST_MEMBER: {
            const ConstEntry *cls = get_entry(u, e.a);
            const ConstEntry *nt = get_entry(u, e.b);
            if (!cls || cls->type != CONST_CLASS)
                ok = false, e2 = "member class ref is not a class const";
            else if (!nt || nt->type != CONST_NAME_TYPE)
                ok = false, e2 = "member nameAndType ref is not a type-7 const";
            else
                ok = check_string_ref(u, nt->a, "member name", &e2) &&
                     check_string_ref(u, nt->b, "member descriptor", &e2);
            break;
        }
        case CONST_NAME_TYPE:
            ok = check_string_ref(u, e.a, "name", &e2) &&
                 check_string_ref(u, e.b, "descriptor", &e2);
            break;
        default:
            ok = false, e2 = "unknown constant type";
            break;
        }
        if (!ok) {
            *err = at + e2;
            return false;
        }
    }
    if (!check_class_ref(u, u.self_class, "self class", err)) return false;
    if (u.super_class >= 0 && !check_class_ref(u, u.super_class, "super class", err))
        return false;
    for (size_t i = 0; i < u.interfaces.size(); i++)
        if (!check_class_ref(u, u.interfaces[i], "interface", err)) return false;

    for (size_t i = 0; i < u.fields.size(); i++) {
        const FieldInfo &f = u.fields[i];
        if (!check_string_ref(u, f.name, "field name", err) ||
            !check_string_ref(u, f.type, "field type", err))
            return false;
        if (f.init < -1 || (f.init >= 0 && (size_t)f.init >= u.bodies.size())) {
            *err = "field #" + std::to_string(i) + ": init body index " +
                   std::to_string(f.init) + " out of range";
            return false;
        }
    }
    for (size_t i = 0; i < u.methods.size(); i++) {
        const MethodInfo &m = u.methods[i];
        if (!check_string_ref(u, m.name, "method name", err) ||
            !check_string_ref(u, m.sig, "method signature", err))
            return false;
        if (m.body < -1 || (m.body >= 0 && (size_t)m.body >= u.bodies.size())) {
            *err = "method #" + std::to_string(i) + ": body index " +
                   std::to_string(m.body) + " out of range";
            return false;
        }
    }
    return true;
}

bool parse_unit(Reader &r, ClassUnit &u, std::string *err)
{
    u.file_offset = (uint32_t)r.pos;
    const uint8_t *magic;
    if (!r.bytes(4, &magic)) return false;
    if (memcmp(magic, "TUFA", 4) != 0) {
        r.pos -= 4;
        return r.fail("bad magic (expected TUFA)");
    }
    if (!r.u32(&u.version) || !r.u32(&u.stamp)) return false;
    if (u.version != 4) {
        return r.fail("unsupported TUFA version " + std::to_string(u.version));
    }
    // Known non-SLRR dialects: CRC 2005 compact, Monster Garage, Street Legal 1.
    if (u.stamp == 0x00013D70 || u.stamp == 0x000108F5 || u.stamp == 0x0001028F) {
        return r.fail("unsupported dialect stamp " + hex32(u.stamp));
    }

    bool has_cons = false, has_clss = false;
    while (r.remaining() > 0) {
        if (r.remaining() < 8)
            return r.fail("trailing bytes (truncated chunk header)");
        const uint8_t *tagp;
        if (!r.bytes(4, &tagp)) return false;
        if (memcmp(tagp, "TUFA", 4) == 0) {
            r.pos -= 4; // next class unit
            break;
        }
        char tag[5] = {0};
        memcpy(tag, tagp, 4);
        uint32_t len;
        if (!r.u32(&len)) return false;
        if ((uint64_t)len > r.remaining())
            return r.fail(std::string("chunk ") + tag + " length exceeds file");
        bool ok = false;
        std::string sub;
        Reader sr{r.data + r.pos, len, 0, &sub};
        if (strcmp(tag, "CONS") == 0) {
            ok = parse_const_pool(sr, u);
            has_cons = true;
        } else if (strcmp(tag, "FILD") == 0) {
            ok = parse_fields(sr, u);
            u.has_fields = true;
        } else if (strcmp(tag, "MTHD") == 0) {
            ok = parse_methods(sr, u);
            u.has_methods = true;
        } else if (strcmp(tag, "CLSS") == 0) {
            ok = parse_class(sr, u);
            has_clss = true;
        } else if (strcmp(tag, "TREE") == 0) {
            ok = parse_tree(sr, u);
        } else {
            return r.fail(std::string("unknown chunk ") + tag);
        }
        if (!ok) {
            if (err) *err = std::string(tag) + ": " + sub;
            return false;
        }
        if (sr.pos != sr.size) {
            return r.fail(std::string("chunk ") + tag + " not fully consumed (" +
                          std::to_string(sr.pos) + " of " + std::to_string(sr.size) + ")");
        }
        r.pos += len;
    }
    if (!has_cons || !has_clss || !u.has_tree)
        return r.fail("missing mandatory chunk (need CONS, CLSS and TREE)");
    std::string verr;
    if (!validate_unit(u, &verr)) {
        if (err) *err = "unit validation: " + verr;
        return false;
    }
    return true;
}

} // namespace

float Instr::as_float() const
{
    uint32_t bits = operand;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

int opcode_operand_size(uint8_t op)
{
    return opcode_size(op);
}

const ConstEntry *ClassUnit::entry(int idx) const
{
    return get_entry(*this, idx);
}

const char *ClassUnit::str(int idx) const
{
    const ConstEntry *e = get_entry(*this, idx);
    return e && e->type == CONST_STRING ? e->str.c_str() : nullptr;
}

std::string ClassUnit::class_name(int class_idx) const
{
    const ConstEntry *e = get_entry(*this, class_idx);
    if (!e || e->type != CONST_CLASS)
        return std::string();
    const char *n = str(e->a);
    return n ? std::string(n) : std::string();
}

std::string ClassUnit::member_class(int member_idx) const
{
    const ConstEntry *e = get_entry(*this, member_idx);
    return e && e->type == CONST_MEMBER ? class_name(e->a) : std::string();
}

std::string ClassUnit::member_name(int member_idx) const
{
    const ConstEntry *e = get_entry(*this, member_idx);
    const ConstEntry *nt = e && e->type == CONST_MEMBER ? get_entry(*this, e->b) : nullptr;
    const char *n = nt && nt->type == CONST_NAME_TYPE ? str(nt->a) : nullptr;
    return n ? std::string(n) : std::string();
}

std::string ClassUnit::member_type(int member_idx) const
{
    const ConstEntry *e = get_entry(*this, member_idx);
    const ConstEntry *nt = e && e->type == CONST_MEMBER ? get_entry(*this, e->b) : nullptr;
    const char *n = nt && nt->type == CONST_NAME_TYPE ? str(nt->b) : nullptr;
    return n ? std::string(n) : std::string();
}

std::string ClassUnit::describe(int idx) const
{
    const ConstEntry *e = get_entry(*this, idx);
    if (!e)
        return "#" + std::to_string(idx) + "?";
    switch (e->type) {
    case CONST_STRING:
        return "\"" + e->str + "\"";
    case CONST_RESOURCE:
        return "resource " + describe(e->a) + " " + hex32((uint32_t)e->b);
    case CONST_CLASS:
        return "class " + describe(e->a);
    case CONST_MEMBER:
        return "member " + member_class(idx) + "::" + member_name(idx) + " " +
               member_type(idx);
    case CONST_NAME_TYPE:
        return "nameType " + describe(e->a) + " " + describe(e->b);
    default:
        return "type" + std::to_string(e->type) + " #" + std::to_string(idx) + "?";
    }
}

bool classfile_parse(const void *data, size_t size, ClassFile *out, std::string *err)
{
    if (err) err->clear();
    out->units.clear();
    if (size == 0) {
        if (err) *err = "empty file";
        return false;
    }
    Reader r{(const uint8_t *)data, size, 0, err};
    while (r.remaining() > 0) {
        ClassUnit u;
        if (!parse_unit(r, u, err))
            return false;
        out->units.push_back(std::move(u));
    }
    if (out->units.empty()) {
        if (err) *err = "no class units";
        return false;
    }
    return true;
}

bool classfile_load(const char *path, ClassFile *out, std::string *err)
{
    std::vector<uint8_t> data;
    if (!fs::read_file(path, &data, err)) return false;
    return classfile_parse(data.data(), data.size(), out, err);
}
