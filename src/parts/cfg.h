// Car-part configuration reader (.cfg).
//
// Grammar recovered from the exe: loader FUN_004f6430 -> line parser
// FUN_004f6240, with the part keyword table at 0x5dab88 (referenced by the
// type_part vtable). SlrrLib's CfgAccess is the field-order cross-check.
//
//   - lines are split on '\n'; CR is whitespace
//   - a line is cut at the first ';', '#' or '/' (so "//" and "/*" comments
//     work, but a '/' inside a path token also ends the line)
//   - the first space/tab-separated token is the key, compared
//     case-sensitively; the rest belongs to the key's handler
//   - "eof" stops parsing; the exe ignores everything after it
//   - unknown keys are ignored by the exe (kept here with known=false)
//   - no include directive exists in the part keyword table
//
// Handler argument formats (exe sscanf strings and SlrrLib descriptors):
//   render   ID [X Y Z [RX RY RZ]]     mesh      ID [X Y Z RX RY RZ]
//   texture  ID                        click     ID|SHAPE [X Y Z [RX RY RZ]]
//   body     ID | X Y Z RX RY RZ MASS SHAPE [S [S S]]
//   slot     X Y Z RX RY RZ SLOT       attach    ID SLOT
//   compatible ID SLOT                 category  I
//   stockpart ID
// IDs are 0x-hex or decimal; slot ids and category are decimal ints.
// attach/compatible lines link to the most recent slot (the exe appends to the
// current slot; SlrrLib scans forward from it).
//
// Part fields fed from this file (notes/13-car-structs.md): texture id +0x84,
// mesh id +0x94, render lines feed the +0x4c/+0x50 arrays (count ctx+0xff8);
// body lines build physics bodies (mass + shape); slot lines become the runtime
// slot array at +0x78 and carry the attach/compatible graph.
//
// Uncertain / exe-lenient spots are marked "shortcut" where they occur.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

struct CfgPosRot {
    float px = 0, py = 0, pz = 0;   // position
    float rx = 0, ry = 0, rz = 0;   // euler rotation
};

struct CfgLine {
    int number = 0;                 // 1-based source line
    std::string key;
    std::vector<std::string> values;    // tokens after the key
    bool known = false;             // handled by the exe's part table
};

struct CfgAttach {
    uint32_t part = 0;              // cfg resource id (pkg << 16 | local)
    int32_t slot = 0;               // slot id on the other part
};

struct CfgRender {
    int line = 0;
    uint32_t id = 0;
    CfgPosRot xform;
    bool has_xform = false;         // 3 or 6 floats were present
};

struct CfgMesh {
    int line = 0;
    uint32_t id = 0;
};

struct CfgTexture {
    int line = 0;
    uint32_t id = 0;
};

struct CfgClick {
    int line = 0;
    std::string target;             // id or primitive/mesh name, as written
    bool has_resource = false;      // target parsed as a non-zero resource id
    uint32_t resource = 0;
    CfgPosRot xform;
    bool has_xform = false;
};

struct CfgBody {
    int line = 0;
    bool resource_form = false;     // "body ID": geometry/mass come from the resource
    uint32_t resource = 0;
    CfgPosRot xform;
    float mass = 0;
    std::string shape;              // resource id text, primitive name or .scx path
    int tail_count = 0;             // optional 1 or 3 trailing floats
    float tail[3] = {0, 0, 0};      // primitive dimensions / mesh scale (createBox args)
    // Record +0x44 bits set by the modifier lines after the body (type table
    // 0x5daba8): `nocollision` 0x4bfba0 -> 0x100.
    uint32_t flags = 0;
};

struct CfgSlot {
    int line = 0;
    CfgPosRot xform;
    int32_t id = 0;
    int32_t slot_type = 0;          // `slottype N`, type slot +0x64 (0x4c0190)
    std::vector<CfgAttach> attaches;
    std::vector<CfgAttach> compatibles;
};

struct CfgStockPart {
    int line = 0;
    uint32_t id = 0;
};

struct CfgFile {
    std::string source;

    std::vector<CfgLine> lines;     // content lines up to and including eof
    bool has_eof = false;
    int eof_line = 0;
    std::vector<int> content_after_eof;    // non-comment lines the exe never sees

    std::vector<CfgRender> renders;
    std::vector<CfgMesh> meshes;
    std::vector<CfgTexture> textures;
    std::vector<CfgClick> clicks;
    std::vector<CfgBody> bodies;
    bool has_category = false;
    int32_t category = 0;
    std::vector<CfgSlot> slots;
    std::vector<CfgAttach> orphan_attaches;      // attach before any slot: ignored
    std::vector<CfgAttach> orphan_compatibles;   // same for compatible
    std::vector<CfgStockPart> stockparts;
};

// Strict typed parse; on failure *err is "source:line: message".
bool cfg_parse(const void *data, size_t size, const char *source, CfgFile *out,
               std::string *err);
bool cfg_load(const char *path, CfgFile *out, std::string *err);
