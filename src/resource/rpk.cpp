#include "resource/rpk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iterator>

namespace {

bool fail(std::string *err, const std::string &msg)
{
    if (err) *err = msg;
    return false;
}

uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

float lef32(const uint8_t *p)
{
    uint32_t bits = le32(p);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

std::string hex(uint64_t v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
    return buf;
}

// One record of the .rpk record format (0x51e6b0), used for the table and for
// the embedded child records of a type-1 blob. off is the record's file offset;
// limit bounds the record bytes (table_end or the end of the parent blob);
// data_min is the first allowed data-area offset.
bool read_record(const uint8_t *file, size_t file_size, size_t off, size_t limit,
                 size_t data_min, RpkRecord *rec, size_t *end, std::string *err)
{
    if ((uint64_t)off + 0x17 > limit)
        return fail(err, "record header overruns at " + hex(off));
    const uint8_t *r = file + off;
    rec->super_id = le32(r);
    rec->id = le32(r + 4);
    rec->type = r[8];
    rec->flags = r[9];
    rec->weight = lef32(r + 0x0a);
    rec->data_offset = le32(r + 0x0e);
    rec->data_size = le32(r + 0x12);
    uint8_t name_len = r[0x16];
    uint64_t q = (uint64_t)off + 0x17;
    if (q + name_len > limit)
        return fail(err, "record name (" + std::to_string(name_len) +
                             " bytes) overruns at " + hex(off));
    rec->name.assign((const char *)file + q, name_len);
    size_t nul = rec->name.find('\0');
    if (nul != std::string::npos) rec->name.resize(nul);
    q += name_len;
    if (rec->flags & 1) {
        if (q + 48 > limit)
            return fail(err, "48-byte extra block overruns at " + hex(off));
        memcpy(rec->extra, file + q, 48);
        rec->has_extra = true;
        q += 48;
    }
    if (rec->data_offset < data_min ||
        (uint64_t)rec->data_offset + rec->data_size > file_size)
        return fail(err, "data range [" + hex(rec->data_offset) + ", " +
                             hex((uint64_t)rec->data_offset + rec->data_size) +
                             ") is outside the file data area");
    *end = (size_t)q;
    return true;
}

} // namespace

bool RpkArchive::parse(const void *data, size_t size, std::string *err)
{
    *this = RpkArchive();
    const uint8_t *p = static_cast<const uint8_t *>(data);
    if (size < 16) return fail(err, "file too small for the RPAK header");
    if (memcmp(p, "RPAK", 4) != 0) return fail(err, "bad magic (expected RPAK)");
    version = le32(p + 4);
    if (version != 0x200)
        return fail(err, "unsupported version " + hex(version) + " (expected 0x200)");
    count_a = le32(p + 8);
    count_b = le32(p + 12);

    uint64_t npkg = (uint64_t)count_a + count_b;
    uint64_t sub_end = 16 + npkg * 0x40;
    if (sub_end + 16 > size)
        return fail(err, "sub-package table (" + std::to_string(npkg) +
                             " entries) overruns the file");
    subpackages.resize((size_t)npkg);
    for (size_t i = 0; i < subpackages.size(); i++) {
        const uint8_t *e = p + 16 + i * 0x40;
        RpkSubpackage &sp = subpackages[i];
        sp.zero = le16(e);
        sp.index = le16(e + 2);
        size_t len = 0;
        while (len < 56 && e[4 + len] != 0) len++;
        sp.name.assign((const char *)e + 4, len);
        sp.tail = le32(e + 0x3c);
    }

    const uint8_t *h = p + sub_end;
    blob_size = le32(h);
    record_count = le32(h + 4);
    header_x1 = le32(h + 8);
    header_x2 = le32(h + 12);
    table_offset = (size_t)(sub_end + 16);
    if ((uint64_t)table_offset + blob_size > size)
        return fail(err, "record table (blob_size " + hex(blob_size) +
                             ") overruns the file");
    table_end = table_offset + blob_size;
    if ((uint64_t)record_count > blob_size / 0x17)
        return fail(err, "record_count " + std::to_string(record_count) +
                             " cannot fit in blob_size " + hex(blob_size));

    bytes_.assign(p, p + size);

    uint64_t off = table_offset;
    records.reserve(record_count);
    for (uint32_t i = 0; i < record_count; i++) {
        RpkRecord rec;
        std::string e;
        if (!read_record(bytes_.data(), size, off, table_end, table_end, &rec, &off, &e))
            return fail(err, "record " + std::to_string(i) + ": " + e);
        records.push_back(std::move(rec));
    }
    if (off != table_end)
        return fail(err, "record table not consumed exactly: stopped at " + hex(off) +
                             ", blob ends at " + hex(table_end));
    return true;
}

// 0x519940. The blob starts with a dword count of leading dwords to skip, then
// the child record count, then the chunk walk. Every read must stay inside the
// record's data range and the walk must end exactly at its end.
bool RpkArchive::parse_instance(const RpkRecord &rec, RpkInstanceData *out,
                                std::string *err) const
{
    if (!out) return fail(err, "parse_instance: no output");
    *out = RpkInstanceData();
    const uint64_t begin = rec.data_offset;
    const uint64_t end_abs = begin + rec.data_size;
    if (end_abs > bytes_.size())
        return fail(err, "instance data [" + hex(begin) + ", " + hex(end_abs) +
                             ") is outside the file");
    if (rec.data_size < 4) return fail(err, "instance data too small for the header");
    const uint8_t *data = bytes_.data() + begin;
    const size_t size = rec.data_size;

    uint64_t p = 4 + (uint64_t)4 * le32(data);
    if (p + 4 > size)
        return fail(err, "instance header overruns the blob (" + std::to_string(size) +
                             " bytes)");
    uint32_t nchild = le32(data + p);
    p += 4;
    if ((uint64_t)nchild > (size - p) / 0x17)
        return fail(err, "child count " + std::to_string(nchild) +
                             " cannot fit in the blob");

    out->children.reserve(nchild);
    for (uint32_t i = 0; i < nchild; i++) {
        RpkRecord child;
        std::string e;
        size_t next = 0;
        if (!read_record(bytes_.data(), bytes_.size(), (size_t)(begin + p),
                         (size_t)end_abs, table_end, &child, &next, &e))
            return fail(err, "child " + std::to_string(i) + ": " + e);
        p = next - begin;
        out->children.push_back(std::move(child));
    }

    while (p < size) {
        if (p + 8 > size) return fail(err, "chunk header overruns the blob");
        uint32_t tag = le32(data + p);
        uint32_t sz = le32(data + p + 4);
        uint64_t q = p + 8;
        if (tag == 0 && sz == 0) {
            // The rest of the blob is the RSD text, starting after 3 dwords.
            if (p + 12 > size) return fail(err, "RSD tail header overruns the blob");
            out->rsd.append((const char *)data + p + 12, size - (size_t)p - 12);
            p = size;
            break;
        }
        uint64_t next = q + sz;
        if (next > size)
            return fail(err, "chunk " + hex(tag) + " (" + std::to_string(sz) +
                                 " bytes) overruns the blob");
        if (tag == 0x00445352) {   // "RSD\0"
            out->rsd.append((const char *)data + q, sz);
        } else if (tag == 0x53594850) {   // "PHYS"
            if (q + 4 > next) return fail(err, "PHYS face count overruns the chunk");
            uint32_t nf = le32(data + q);
            q += 4;
            if (q + (uint64_t)nf * 28 > next)
                return fail(err, "PHYS faces (" + std::to_string(nf) +
                                     ") overrun the chunk");
            uint32_t base_vert = (uint32_t)(out->verts.size() / 3);
            size_t face0 = out->faces.size();
            out->faces.resize(face0 + nf);
            for (uint32_t i = 0; i < nf; i++) {
                RpkPhysFace &f = out->faces[face0 + i];
                f.mat = le32(data + q);
                f.v[0] = le32(data + q + 4) + base_vert;
                f.v[1] = le32(data + q + 8) + base_vert;
                f.v[2] = le32(data + q + 12) + base_vert;
                f.n[0] = lef32(data + q + 16);
                f.n[1] = lef32(data + q + 20);
                f.n[2] = lef32(data + q + 24);
                q += 28;
            }
            if (q + 4 > next) return fail(err, "PHYS vertex count overruns the chunk");
            uint32_t nv = le32(data + q);
            q += 4;
            if (q + (uint64_t)nv * 12 > next)
                return fail(err, "PHYS vertices (" + std::to_string(nv) +
                                     ") overrun the chunk");
            out->verts.reserve(out->verts.size() + (size_t)nv * 3);
            for (uint32_t i = 0; i < nv; i++) {
                out->verts.push_back(lef32(data + q));
                out->verts.push_back(lef32(data + q + 4));
                out->verts.push_back(lef32(data + q + 8));
                q += 12;
            }
            // Indices are chunk-local; the exe ignores sz, so require it to be
            // exactly what the parsed arrays used.
            for (size_t i = face0; i < out->faces.size(); i++)
                for (int k = 0; k < 3; k++)
                    if (out->faces[i].v[k] - base_vert >= nv)
                        return fail(err, "PHYS face index out of range");
            // sz is the payload size (the 8-byte chunk header is not counted).
            if (q - (p + 8) != sz)
                return fail(err, "PHYS size " + std::to_string(sz) +
                                     " != parsed " + std::to_string(q - (p + 8)));
            out->phys_chunks++;
        } else if (tag == 0x50545845) {   // "EXTP"
            if (sz % 4) return fail(err, "EXTP size " + std::to_string(sz) +
                                               " is not a multiple of 4");
            for (uint64_t i = q; i < next; i += 4) out->extp.push_back(le32(data + i));
        }
        // POLY (0x594c4f50) and unknown chunks are skipped by their size.
        p = next;
    }
    if (p != size)
        return fail(err, "instance walk ended at " + std::to_string(p) + ", blob is " +
                             std::to_string(size));
    return true;
}

// 0x519940: RSD descriptor line rule. The key runs to the first whitespace;
// `gametype` is hex, `params` is the rest of the line.
static bool rsd_ws(char c) { return c == ' ' || (c >= 9 && c <= 13); }

bool rpk_rsd_fields(const std::string &rsd, uint32_t *gametype, std::string *params)
{
    bool has_type = false;
    size_t pos = 0;
    while (pos < rsd.size()) {
        size_t end = rsd.find('\n', pos);
        if (end == std::string::npos) end = rsd.size();
        std::string line = rsd.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t k = 0;
        while (k < line.size() && !rsd_ws(line[k])) k++;
        std::string key = line.substr(0, k);
        while (k < line.size() && rsd_ws(line[k])) k++;
        if (key == "gametype") {
            *gametype = (uint32_t)strtoul(line.c_str() + k, nullptr, 16);
            has_type = true;
        } else if (key == "params") {
            *params = line.substr(k);
        }
        pos = end + 1;
    }
    return has_type;
}

// 0x519940: child records are embedded in a type-1 blob in record order;
// 0x4fe6f0 then recurses into the type-1 ones depth first.
bool RpkArchive::map_objects(const RpkRecord &rec, std::vector<RpkMapObject> *out,
                             std::string *err) const
{
    if (!out) return fail(err, "map_objects: no output");
    RpkInstanceData inst;
    std::string e;
    if (!parse_instance(rec, &inst, &e))
        return fail(err, "record " + rec.name + ": " + e);
    uint32_t gametype = 0;
    std::string params;
    if (rpk_rsd_fields(inst.rsd, &gametype, &params)) {
        RpkMapObject obj;
        obj.rec = rec;
        obj.gametype = gametype;
        obj.params = std::move(params);
        obj.phys = !inst.faces.empty();
        obj.children = inst.children.size();
        out->push_back(std::move(obj));
    }
    for (const RpkRecord &child : inst.children)
        if (child.type == 1 && !map_objects(child, out, err)) return false;
    return true;
}

bool RpkArchive::load(const char *path, std::string *err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(err, std::string("cannot open ") + path);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (f.bad()) return fail(err, std::string("cannot read ") + path);
    return parse(data.data(), data.size(), err);
}

const RpkRecord *RpkArchive::find(uint32_t id) const
{
    for (const RpkRecord &r : records)
        if (r.id == id) return &r;
    return nullptr;
}

const RpkRecord *RpkArchive::find_name(const std::string &name) const
{
    for (const RpkRecord &r : records)
        if (r.name == name) return &r;
    return nullptr;
}
