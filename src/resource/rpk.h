// RPAK resource package reader (format: notes/01-rpk-rdb-format.md).
//
// Layout, little-endian:
//   "RPAK" version:u32 count_a:u32 count_b:u32
//   (count_a+count_b) * 0x40 sub-package entries
//   blobSize:u32 recordCount:u32 x1:u32 x2:u32
//   record table (blobSize bytes), then the data area to EOF.
// Record: superId:u32 id:u32 type:u8 flags:u8 weight:f32
//         dataOffset:u32 dataSize:u32 nameLen:u8 name[nameLen] [extra:48 if flags&1]
// dataOffset is absolute from the start of the file.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

struct RpkSubpackage {
    uint16_t zero = 0;
    uint16_t index = 0;   // package number used in the high 16 bits of ids
    uint32_t tail = 0;    // copied by the loader, meaning unknown
    std::string name;     // relative package path, e.g. "cars\\racers.rpk"
};

struct RpkRecord {
    uint32_t super_id = 0;    // parent/class resource, pkg<<16 | local
    uint32_t id = 0;
    uint8_t type = 0;         // resource type code (matches typeof in the .rdb)
    uint8_t flags = 0;        // bit0: a 48-byte block follows the name
    float weight = 0.0f;      // f32 at +0x0a, meaning unknown
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    std::string name;
    bool has_extra = false;
    uint8_t extra[48] = {};
};

// One triangle of a type-1 blob's PHYS chunk (0x519940): material index and
// three vertex indices into the chunk's vertex array (verts/3).
struct RpkPhysFace {
    uint32_t mat = 0;
    uint32_t v[3] = {};
    float n[3] = {};
};

// Parsed data of a type-1 (game instance) record. parse_instance walks only
// the direct children; callers recurse into the type-1 ones.
struct RpkInstanceData {
    std::vector<RpkRecord> children;   // embedded records, same layout as the table
    std::vector<RpkPhysFace> faces;    // all PHYS chunks, indices offset into verts
    std::vector<float> verts;          // xyz triples, metres in the world frame
    std::vector<uint32_t> extp;        // EXTP precache ids
    std::string rsd;                   // RSD text chunks concatenated
    int phys_chunks = 0;
};

class RpkArchive {
public:
    // Strict parse: header, version, sub-package table, record table and every
    // record data range must stay inside the file; the record table must be
    // consumed exactly.
    bool parse(const void *data, size_t size, std::string *err);
    bool load(const char *path, std::string *err);

    uint32_t version = 0;
    uint32_t count_a = 0, count_b = 0;
    uint32_t blob_size = 0, record_count = 0;
    uint32_t header_x1 = 0, header_x2 = 0;   // read by the loader, meaning unknown
    std::vector<RpkSubpackage> subpackages;

    size_t table_offset = 0;   // record table start
    size_t table_end = 0;      // record table end = data area start
    std::vector<RpkRecord> records;

    // 0x519940 ResourceInstance_Game__loadData: embedded child records, PHYS
    // triangle soup, EXTP ids and RSD text of a type-1 record's data.
    bool parse_instance(const RpkRecord &rec, RpkInstanceData *out,
                        std::string *err) const;

    size_t size() const { return bytes_.size(); }
    const RpkRecord *find(uint32_t id) const;
    const RpkRecord *find_name(const std::string &name) const;
    const uint8_t *data(const RpkRecord &rec) const { return bytes_.data() + rec.data_offset; }

private:
    std::vector<uint8_t> bytes_;
};
