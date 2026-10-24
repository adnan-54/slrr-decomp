// Class skeleton writer (stage 3 of the .java -> TUFA compiler): header, CONS,
// FILD, MTHD, CLSS and zero-length TREE bodies. Format: FORMAT.md "SLRR TUFA";
// the reader lives in script/classfile.h. Bodies are stage 4.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "script/classfile.h"
#include "script/compiler/decl.h"
#include "script/compiler/lexer.h"

// Resolved member/class symbols used to name the constants a body refers to.
struct FieldSym {
    std::string name, desc;
    bool is_static = false;
};
struct MethodSym {
    std::string name, sig;
    bool is_static = false;
};
struct ClassSym {
    std::string name, super;
    std::vector<FieldSym> fields;
    std::vector<MethodSym> methods;
};

// Named classes the source refers to. The exe resolves them through its class
// table (notes/03); here it is fed from the shipped .class files.
class ClassIndex {
public:
    void add(const ClassUnit &u);
    const ClassSym *find(const std::string &name) const;

private:
    std::vector<ClassSym> classes_;
    std::map<std::string, size_t> by_name_;
};

// Compiles one parsed source unit. `tokens` is the stage-1 lex (body ranges
// index it), `index` supplies classes/superclasses the source names.
bool compile_unit(const JavaUnit &src, const std::vector<Token> &tokens,
                  const ClassIndex &index, ClassFile *out, std::string *err);

// Serialises class units to TUFA bytes.
bool classfile_write(const ClassFile &file, std::vector<uint8_t> *out);
