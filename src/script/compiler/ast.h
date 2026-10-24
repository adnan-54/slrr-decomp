// AST for method bodies and field initializers (stage 2 of the .java -> TUFA
// compiler). Operator/value node kinds reuse the TUFA TREE node-kind enum
// (FORMAT.md "TREE") so stage 4 can emit one node kind per AST node; primaries
// are separate kinds because the TREE stream encodes them with instructions
// (0x1b/0x1c/0x19/...) rather than a node kind. Every node keeps the source
// line of the token it was built from; blocks also keep their closing line
// (0x15's line field is the block end).
#pragma once
#include <memory>
#include <string>
#include <vector>

enum class ExprKind : int {
    // TREE node-kind enum, 0..46 (FORMAT.md): 12/29/43 unobserved, not used.
    Ternary = 0,
    LogOr = 1,
    LogAnd = 2,
    BitOr = 3,
    BitXor = 4,
    BitAnd = 5,
    Ne = 6,
    Eq = 7,
    Ge = 8,
    Le = 9,
    Gt = 10,
    Lt = 11,
    Ushr = 12,
    Shr = 13,
    Shl = 14,
    Sub = 15,
    Add = 16,
    Mod = 17,
    Div = 18,
    Mul = 19,
    BitNot = 20,
    Not = 21,
    Neg = 22,
    PostInc = 23,
    PostDec = 24,
    PreInc = 25,
    PreDec = 26,
    Call = 27,
    Const = 28, // unwrapped literal prefix in TREE; ast literals below
    New = 30,
    NewArray = 31,
    InstanceOf = 32,
    Cast = 33,
    CtorChain = 34,
    Assign = 35,
    MulAssign = 36,
    DivAssign = 37,
    ModAssign = 38,
    AddAssign = 39,
    SubAssign = 40,
    ShlAssign = 41,
    ShrAssign = 42,
    UshrAssign = 43,
    BitAndAssign = 44,
    BitXorAssign = 45,
    BitOrAssign = 46,

    // Primaries and access chains, encoded by TREE instructions, not kinds.
    Name = 100,
    Member,
    Index,
    This,
    Super,
    Null,
    IntLit,
    FloatLit,
    StringLit,
    CharLit,
    BoolLit,
    ResourceLit,
    ArrayInit,
    ClassLit,
    UnaryPlus,
};

// kids meaning per kind:
//   binary/assign  [left, right] (assign: [target, value])
//   ternary        [cond, then, else]
//   unary/postfix  [operand]
//   call           [receiver?] then args; receiver absent for unqualified calls
//   member/index   [object] / [array, index]
//   new            args; text = type
//   newarray       dimension exprs; text = type with one [] per dimension;
//                  flag = an initializer follows, stored as the last kid
//   cast/instanceof [operand]; text = type
//   arrayinit      elements
//   literals       text = source spelling (BoolLit: flag = value)
struct Expr {
    ExprKind kind = ExprKind::Null;
    int line = 0;
    std::string text;
    bool flag = false;
    std::vector<std::unique_ptr<Expr>> kids;
};

struct Stmt;
struct SwitchCase {
    bool is_default = false;
    int line = 0;
    std::unique_ptr<Expr> label; // null for default
    std::vector<std::unique_ptr<Stmt>> stmts;
};

enum class StmtKind {
    Block,
    Empty,
    Expr,
    LocalDecl,
    If,
    While,
    DoWhile,
    For,
    Switch,
    Break,
    Continue,
    Return,
    CtorCall,
};

struct DeclAst {
    std::string name;
    int dims = 0; // dimensions written after the name: `int x[]`
    int line = 0;
    std::unique_ptr<Expr> init;
};

struct Stmt {
    StmtKind kind = StmtKind::Empty;
    int line = 0;
    int end_line = 0; // Block/Switch closing brace

    std::vector<std::unique_ptr<Stmt>> stmts; // Block
    std::unique_ptr<Expr> expr;               // Expr/If/While/DoWhile/Switch, Return value

    // LocalDecl, and the init clause of a For
    std::string type;
    std::vector<DeclAst> decls;

    std::unique_ptr<Stmt> then_stmt, else_stmt, body;
    std::unique_ptr<Stmt> init_stmt; // For: local declaration, else null
    std::vector<std::unique_ptr<Expr>> init_exprs, update_exprs;
    bool has_cond = false; // For: whether `expr` is the condition

    std::vector<SwitchCase> cases; // Switch

    std::string label; // Break/Continue

    bool is_super = false;                     // CtorCall
    std::vector<std::unique_ptr<Expr>> args;   // CtorCall
};

struct BodyAst {
    int close_line = 0;                 // method: line of the closing brace
    std::unique_ptr<Expr> init;         // field initializer
    std::vector<std::unique_ptr<Stmt>> stmts; // method body
};
