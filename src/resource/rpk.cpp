#include "resource/rpk.h"

#include <stdio.h>
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
        if (off + 0x17 > table_end)
            return fail(err, "record " + std::to_string(i) +
                                 " header overruns the record table at " + hex(off));
        const uint8_t *r = bytes_.data() + off;
        RpkRecord rec;
        rec.super_id = le32(r);
        rec.id = le32(r + 4);
        rec.type = r[8];
        rec.flags = r[9];
        rec.weight = lef32(r + 0x0a);
        rec.data_offset = le32(r + 0x0e);
        rec.data_size = le32(r + 0x12);
        uint8_t name_len = r[0x16];
        uint64_t q = off + 0x17;
        if (q + name_len > table_end)
            return fail(err, "record " + std::to_string(i) + " name (" +
                                 std::to_string(name_len) + " bytes) overruns the record table");
        rec.name.assign((const char *)bytes_.data() + q, name_len);
        size_t nul = rec.name.find('\0');
        if (nul != std::string::npos) rec.name.resize(nul);
        q += name_len;
        if (rec.flags & 1) {
            if (q + 48 > table_end)
                return fail(err, "record " + std::to_string(i) +
                                     " 48-byte extra block overruns the record table");
            memcpy(rec.extra, bytes_.data() + q, 48);
            rec.has_extra = true;
            q += 48;
        }
        if (rec.data_offset < table_end || (uint64_t)rec.data_offset + rec.data_size > size)
            return fail(err, "record " + std::to_string(i) + " data range [" +
                                 hex(rec.data_offset) + ", " +
                                 hex((uint64_t)rec.data_offset + rec.data_size) +
                                 ") is outside the data area");
        records.push_back(std::move(rec));
        off = q;
    }
    if (off != table_end)
        return fail(err, "record table not consumed exactly: stopped at " + hex(off) +
                             ", blob ends at " + hex(table_end));
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
