// TUFA compiled-script (.class) container parser.
// Layout: notes/03-script-vm.md, notes/05-thread-run-opcodes.md and the community
// FORMAT.md shipped with bin/tools/slrr-script-decompiler (single source of truth).
//
// A file is one or more class units: "TUFA" version:u32 stamp:u32 chunk*
// chunk = tag:4cc byteLen:u32 data[byteLen]; order CONS [FILD] [MTHD] CLSS TREE.
// Only the SLRR encoding is handled; the CRC/MG/SL1 variants are rejected loudly.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

enum ConstType {
    CONST_STRING    = 0,   // len-prefixed chars + NUL
    CONST_RESOURCE  = 3,   // a = name string idx, b = resource id
    CONST_CLASS     = 4,   // a = name string idx
    CONST_MEMBER    = 5,   // a = class const idx, b = nameAndType const idx
    CONST_NAME_TYPE = 7,   // a = name string idx, b = descriptor string idx
};

struct ConstEntry {
    uint32_t type = 0;
    std::string str;       // type 0 payload
    int32_t a = 0, b = 0;  // type-dependent, see ConstType
};

// One TREE instruction: op:u8 srcLine:u16 payload[size(op)].
// The TREE body is a postfix AST stream; payload is a const index, a node kind,
// an integer, raw float bits or a relative jump, depending on op.
struct Instr {
    uint8_t op = 0;
    uint16_t line = 0;
    uint32_t operand = 0;     // 0 when !has_operand
    bool has_operand = false;
    uint32_t file_offset = 0; // offset in the TREE chunk payload
    float as_float() const;
};

struct Body {
    std::vector<Instr> instrs;
    uint32_t byte_length = 0; // instruction bytes (excluding the 4-byte instr count)
};

struct FieldInfo {
    bool is_static = false;   // first FILD section
    uint32_t flags = 0;       // 0x01 public, 0x08 static, 0x20 final, 0x40 native (observed)
    int32_t name = -1;        // string const
    int32_t type = -1;        // descriptor string const
    int32_t init = -1;        // TREE body index, -1 = no initializer
};

struct MethodInfo {
    bool is_static = false;   // first MTHD section
    uint32_t flags = 0;
    int32_t name = -1;        // string const
    int32_t sig = -1;         // descriptor string const
    int32_t body = -1;        // TREE body index, -1 = no body
    int32_t locals = 0;       // local slots including `this` for instance methods
};

struct ClassUnit {
    uint32_t file_offset = 0;
    uint32_t version = 0;
    uint32_t stamp = 0;

    std::vector<ConstEntry> consts;
    bool has_fields = false, has_methods = false, has_tree = false;
    std::vector<FieldInfo> fields;     // static section first, then instance
    std::vector<MethodInfo> methods;   // static section first, then instance

    int32_t clss_unknown = -1;         // observed -1
    int32_t interface_flag = 0;
    int32_t self_class = -1;           // class const index
    int32_t super_class = -1;          // class const index or -1
    std::vector<int32_t> interfaces;   // class const indices

    std::vector<Body> bodies;          // TREE bodies, indexed by FieldInfo::init / MethodInfo::body

    // Safe constant-pool accessors: nullptr/"" on a bad index or wrong type.
    const ConstEntry *entry(int idx) const;
    const char *str(int idx) const;
    std::string class_name(int class_idx) const;
    std::string member_class(int member_idx) const;
    std::string member_name(int member_idx) const;
    std::string member_type(int member_idx) const;
    std::string describe(int idx) const;   // human-readable constant, for dumps
};

struct ClassFile {
    std::vector<ClassUnit> units;
};

// Strict parse: every chunk must be consumed exactly and the file must end on a
// unit boundary. Unknown opcodes, bad const indices and unsupported dialects error.
bool classfile_parse(const void *data, size_t size, ClassFile *out, std::string *err);
bool classfile_load(const char *path, ClassFile *out, std::string *err);

// Operand payload size after [op:u8][line:u16]; -1 = unknown opcode.
int opcode_operand_size(uint8_t op);
