#include "resource/rdb.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace {

bool fail(std::string *err, const std::string &msg)
{
    if (err) *err = msg;
    return false;
}

bool starts_with(const std::string &s, const char *prefix)
{
    size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool ends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string hex32(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

bool parse_u32(const std::string &s, int base, uint32_t *out)
{
    if (s.empty()) return false;
    errno = 0;
    char *end = nullptr;
    unsigned long long v = strtoull(s.c_str(), &end, base);
    if (errno || end == s.c_str() || *end != '\0' || v > 0xffffffffULL) return false;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '-') return false;
    *out = (uint32_t)v;
    return true;
}

bool parse_i32(const std::string &s, int32_t *out)
{
    if (s.empty()) return false;
    errno = 0;
    char *end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (errno || end == s.c_str() || *end != '\0') return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    *out = (int32_t)v;
    return true;
}

bool parse_double(const std::string &s, double *out)
{
    if (s.empty()) return false;
    errno = 0;
    char *end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (errno || end == s.c_str() || *end != '\0') return false;
    *out = v;
    return true;
}

// Parses one .res block. last_res stays at the most recent resource so the
// following .rsd block can attach to it.
bool process_block(Rdb &db, const std::string &name,
                   const std::vector<std::string> &lines, size_t &last_res,
                   std::string *err)
{
    if (name == "external_links") {
        if (db.has_external_links) return fail(err, "duplicate external_links block");
        db.has_external_links = true;
        for (const std::string &line : lines)
            if (!line.empty()) db.external_links.push_back(line);
        return true;
    }

    if (ends_with(name, ".res")) {
        RdbResource res;
        res.source_file = name;
        int seen = 0;   // 1 typeof, 2 superid, 4 typeid, 8 alias
        for (const std::string &line : lines) {
            if (line.empty()) continue;
            if (starts_with(line, "typeof\t")) {
                if (!parse_u32(line.substr(7), 10, &res.type))
                    return fail(err, name + ": bad typeof '" + line.substr(7) + "'");
                seen |= 1;
            } else if (starts_with(line, "superid\t")) {
                if (!parse_u32(line.substr(8), 16, &res.super_id))
                    return fail(err, name + ": bad superid '" + line.substr(8) + "'");
                seen |= 2;
            } else if (starts_with(line, "typeid\t")) {
                if (!parse_u32(line.substr(7), 16, &res.id))
                    return fail(err, name + ": bad typeid '" + line.substr(7) + "'");
                seen |= 4;
            } else if (starts_with(line, "alias\t")) {
                res.alias = line.substr(6);
                seen |= 8;
            } else if (starts_with(line, "isparentcompatible\t")) {
                if (!parse_double(line.substr(19), &res.parent_compatible))
                    return fail(err, name + ": bad isparentcompatible '" +
                                        line.substr(19) + "'");
            } else if (starts_with(line, "rpropcount\t")) {
                if (!parse_u32(line.substr(11), 10, &res.prop_count))
                    return fail(err, name + ": bad rpropcount '" + line.substr(11) + "'");
                res.has_prop_count = true;
            } else if (starts_with(line, "rprop ")) {
                // rprop <index> <type> <name>, all decimal.
                size_t a = 6, b = line.find(' ', a);
                if (b == std::string::npos)
                    return fail(err, name + ": truncated rprop line '" + line + "'");
                RdbProperty prop;
                if (!parse_i32(line.substr(a, b - a), &prop.index))
                    return fail(err, name + ": bad rprop index '" + line.substr(a, b - a) + "'");
                a = b + 1;
                b = line.find(' ', a);
                std::string type_tok = b == std::string::npos ? line.substr(a)
                                                             : line.substr(a, b - a);
                if (!parse_i32(type_tok, &prop.type))
                    return fail(err, name + ": bad rprop type '" + type_tok + "'");
                if (b != std::string::npos) prop.name = line.substr(b + 1);
                res.props.push_back(std::move(prop));
            }
            // Unknown fields are ignored; the schema is open-ended.
        }
        if (seen != 15) {
            std::string missing;
            if (!(seen & 1)) missing += " typeof";
            if (!(seen & 2)) missing += " superid";
            if (!(seen & 4)) missing += " typeid";
            if (!(seen & 8)) missing += " alias";
            return fail(err, name + ": missing required field(s):" + missing);
        }
        last_res = db.resources.size();
        db.resources.push_back(std::move(res));
        return true;
    }

    if (ends_with(name, ".rsd")) {
        if (last_res == (size_t)-1)
            return fail(err, name + ": .rsd block before any .res block");
        RdbResource &res = db.resources[last_res];
        if (res.has_rsd)
            return fail(err, name + ": second .rsd block for " + res.source_file);
        std::string text;
        for (size_t i = 0; i < lines.size(); i++) {
            if (i) text += "\r\n";
            text += lines[i];
        }
        if (!lines.empty()) text += "\r\n";
        res.rsd = std::move(text);
        res.has_rsd = true;
        return true;
    }

    // Unknown block names are ignored.
    return true;
}

} // namespace

bool Rdb::parse(const void *data, size_t size, std::string *err)
{
    *this = Rdb();
    if (size == 0) return fail(err, "empty database");
    const char *text = static_cast<const char *>(data);
    std::vector<std::string> block_lines;
    std::string block_name;
    bool in_block = false;
    size_t last_res = (size_t)-1;
    size_t line_no = 0;

    auto finish = [&]() -> bool {
        bool ok = process_block(*this, block_name, block_lines, last_res, err);
        block_lines.clear();
        return ok;
    };

    size_t start = 0;
    for (;;) {
        const void *hit = start < size ? memchr(text + start, '\n', size - start) : nullptr;
        size_t nl = hit ? (size_t)((const char *)hit - text) : std::string::npos;
        size_t end = nl == std::string::npos ? size : nl;
        std::string line(text + start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line_no++;

        if (starts_with(line, "<FILE ")) {
            if (in_block) {
                return fail(err, "line " + std::to_string(line_no) +
                                     ": nested <FILE> inside a block");
            }
            if (line.size() < 8 || line.compare(line.size() - 2, 2, " >") != 0)
                return fail(err, "line " + std::to_string(line_no) +
                                     ": malformed <FILE ... > header");
            block_name = line.substr(6, line.size() - 8);
            block_lines.clear();
            in_block = true;
        } else if (line == "</FILE>") {
            if (!in_block)
                return fail(err, "line " + std::to_string(line_no) + ": unmatched </FILE>");
            if (!finish()) return false;
            in_block = false;
        } else if (in_block) {
            block_lines.push_back(line);
        }
        // Text outside blocks is comment material and is ignored.

        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (in_block)
        return fail(err, "unterminated <FILE " + block_name + "> block");

    std::unordered_map<uint32_t, size_t> seen;
    for (size_t i = 0; i < resources.size(); i++) {
        auto it = seen.find(resources[i].id);
        if (it != seen.end())
            return fail(err, "duplicate typeid " + hex32(resources[i].id) + " in " +
                                 resources[it->second].source_file + " and " +
                                 resources[i].source_file);
        seen.emplace(resources[i].id, i);
    }
    return true;
}

bool Rdb::load(const char *path, std::string *err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(err, std::string("cannot open ") + path);
    std::vector<char> data((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    if (f.bad()) return fail(err, std::string("cannot read ") + path);
    return parse(data.data(), data.size(), err);
}

const RdbResource *Rdb::find(uint32_t id) const
{
    for (const RdbResource &r : resources)
        if (r.id == id) return &r;
    return nullptr;
}

const RdbResource *Rdb::find_alias(const std::string &alias) const
{
    for (const RdbResource &r : resources)
        if (r.alias == alias) return &r;
    return nullptr;
}

std::string Rdb::type_name(uint32_t type) const
{
    for (const RdbResource &r : resources) {
        if (r.type != type) continue;
        if (starts_with(r.alias, "typ_") || starts_with(r.alias, "ins_") ||
            starts_with(r.alias, "res_"))
            return r.alias;
    }
    return std::string();
}
