// Car-part .cfg reader (grammar and exe addresses: parts/cfg.h).
#include "parts/cfg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

namespace {

// The exe tokenizes with strtok_r(line, " \r\n\t", ...) (0x5a7220).
bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::vector<std::string> split_ws(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) i++;
        size_t begin = i;
        while (i < s.size() && !is_space(s[i])) i++;
        if (i > begin) out.push_back(s.substr(begin, i - begin));
    }
    return out;
}

bool parse_float(const std::string &s, float *out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    float v = strtof(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    *out = v;
    return true;
}

bool parse_i32(const std::string &s, int32_t *out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0' ||
        v < INT32_MIN || v > INT32_MAX)
        return false;
    *out = (int32_t)v;
    return true;
}

// Resource ids are written as 0xHEX in every shipped cfg; decimal is accepted
// because the exe parses them with its own scanf ("%d"). Hand-written car cfgs
// also use script literals with a trailing 'r' (0x...r); the exe's numeric
// scan stops before the suffix, so accept it too.
bool try_id(const std::string &s, uint32_t *out)
{
    if (s.empty() || s[0] == '-') return false;
    const char *p = s.c_str();
    int base = 10;
    if (s.size() > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    if (!*p) return false;
    char *end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(p, &end, base);
    if (errno != 0 || end == p || v > 0xffffffffULL) return false;
    if (*end != '\0' && !((*end == 'r' || *end == 'R') && end[1] == '\0'))
        return false;
    *out = (uint32_t)v;
    return true;
}

bool known_key(const std::string &k)
{
    return k == "render" || k == "mesh" || k == "texture" || k == "click" ||
           k == "body" || k == "category" || k == "slot" || k == "attach" ||
           k == "compatible" || k == "stockpart" || k == "eof";
}

struct Parser {
    CfgFile *cfg;
    std::string *err;
    std::string source;

    Parser(CfgFile *c, const char *src, std::string *e)
        : cfg(c), err(e), source(src ? src : "<memory>")
    {
    }

    bool fail(int line, const std::string &msg)
    {
        if (err)
            *err = source + ":" + std::to_string(line) + ": " + msg;
        return false;
    }
    bool fail(const CfgLine &ln, const std::string &msg) { return fail(ln.number, msg); }

    bool id_at(const CfgLine &ln, size_t i, const char *what, uint32_t *out)
    {
        if (!try_id(ln.values[i], out))
            return fail(ln, std::string(what) + ": expected 0xhex or decimal id, got \"" +
                               ln.values[i] + "\"");
        return true;
    }
    bool int_at(const CfgLine &ln, size_t i, const char *what, int32_t *out)
    {
        if (!parse_i32(ln.values[i], out))
            return fail(ln, std::string(what) + ": expected decimal int, got \"" +
                               ln.values[i] + "\"");
        return true;
    }
    bool float_at(const CfgLine &ln, size_t i, const char *what, float *out)
    {
        if (!parse_float(ln.values[i], out))
            return fail(ln, std::string(what) + ": expected float, got \"" +
                               ln.values[i] + "\"");
        return true;
    }
    bool transform(const CfgLine &ln, size_t first, int floats, CfgPosRot *x)
    {
        static const char *names[6] = {"x", "y", "z", "rx", "ry", "rz"};
        float *dst[6] = {&x->px, &x->py, &x->pz, &x->rx, &x->ry, &x->rz};
        for (int i = 0; i < floats; i++)
            if (!float_at(ln, first + (size_t)i, names[i], dst[i])) return false;
        return true;
    }

    bool parse_known(const CfgLine &ln)
    {
        const std::vector<std::string> &v = ln.values;
        const size_t n = v.size();
        const std::string &k = ln.key;

        if (k == "render") {
            if (n == 0) return fail(ln, "render expects a resource id");
            CfgRender r;
            r.line = ln.number;
            if (!id_at(ln, 0, "render id", &r.id)) return false;
            if (n != 1) {
                if (n < 4 || n == 5 || n == 6)
                    return fail(ln, "render expects id [x y z [rx ry rz]], got " +
                                        std::to_string(n) + " value(s)");
                if (!transform(ln, 1, n >= 7 ? 6 : 3, &r.xform)) return false;
                r.has_xform = true;
                // shortcut: floats past the six are ignored, matching the exe's
                // sscanf; one shipped cfg has render id + 7 floats.
            }
            cfg->renders.push_back(r);
            return true;
        }

        if (k == "mesh") {
            if (n != 1 && n != 7)
                return fail(ln, "mesh expects id or id plus 6 floats, got " +
                                    std::to_string(n) + " value(s)");
            CfgMesh m;
            m.line = ln.number;
            if (!id_at(ln, 0, "mesh id", &m.id)) return false;
            // the optional six floats are in SlrrLib's descriptor but the exe
            // handler only registers the id (FUN_004c04c0).
            cfg->meshes.push_back(m);
            return true;
        }

        if (k == "texture") {
            if (n != 1) return fail(ln, "texture expects one resource id");
            CfgTexture t;
            t.line = ln.number;
            if (!id_at(ln, 0, "texture id", &t.id)) return false;
            cfg->textures.push_back(t);
            return true;
        }

        if (k == "click") {
            if (n == 0) return fail(ln, "click expects an id or shape name");
            CfgClick c;
            c.line = ln.number;
            c.target = v[0];
            uint32_t id = 0;
            if (try_id(v[0], &id) && id != 0) {
                c.has_resource = true;
                c.resource = id;
            }
            if (n != 1) {
                if (n < 4 || n == 5 || n == 6)
                    return fail(ln, "click expects id|shape [x y z [rx ry rz]], got " +
                                        std::to_string(n) + " value(s)");
                if (!transform(ln, 1, n >= 7 ? 6 : 3, &c.xform)) return false;
                c.has_xform = true;
            }
            cfg->clicks.push_back(c);
            return true;
        }

        if (k == "body") {
            CfgBody b;
            b.line = ln.number;
            if (n == 1) {
                // "body ID": the resource carries the shape and mass.
                b.resource_form = true;
                if (!id_at(ln, 0, "body resource id", &b.resource)) return false;
            } else {
                if (n != 8 && n != 9 && n != 11)
                    return fail(ln, "body expects x y z rx ry rz mass shape "
                                    "[s [s s]] or a resource id, got " +
                                        std::to_string(n) + " value(s)");
                if (!transform(ln, 0, 6, &b.xform)) return false;
                if (!float_at(ln, 6, "body mass", &b.mass)) return false;
                b.shape = v[7];
                b.tail_count = (int)n - 8;
                for (int i = 0; i < b.tail_count; i++)
                    if (!float_at(ln, 8 + (size_t)i, "body size", &b.tail[i]))
                        return false;
            }
            cfg->bodies.push_back(b);
            return true;
        }

        if (k == "category") {
            if (n != 1) return fail(ln, "category expects one int");
            if (!int_at(ln, 0, "category", &cfg->category)) return false;
            cfg->has_category = true;
            return true;
        }

        if (k == "slot") {
            if (n != 7)
                return fail(ln, "slot expects x y z rx ry rz slot-id, got " +
                                    std::to_string(n) + " value(s)");
            CfgSlot s;
            s.line = ln.number;
            if (!transform(ln, 0, 6, &s.xform)) return false;
            if (!int_at(ln, 6, "slot id", &s.id)) return false;
            cfg->slots.push_back(s);
            return true;
        }

        if (k == "attach" || k == "compatible") {
            if (n != 2)
                return fail(ln, k + " expects part-id slot-id, got " +
                                    std::to_string(n) + " value(s)");
            CfgAttach a;
            if (!id_at(ln, 0, (k + " part id").c_str(), &a.part)) return false;
            if (!int_at(ln, 1, (k + " slot id").c_str(), &a.slot)) return false;
            bool attach = k == "attach";
            if (cfg->slots.empty()) {
                // The exe only links to a current slot; orphan lines are ignored.
                if (attach) cfg->orphan_attaches.push_back(a);
                else cfg->orphan_compatibles.push_back(a);
            } else if (attach) {
                cfg->slots.back().attaches.push_back(a);
            } else {
                cfg->slots.back().compatibles.push_back(a);
            }
            return true;
        }

        if (k == "stockpart") {
            if (n != 1) return fail(ln, "stockpart expects one resource id");
            CfgStockPart s;
            s.line = ln.number;
            if (!id_at(ln, 0, "stockpart id", &s.id)) return false;
            cfg->stockparts.push_back(s);
            return true;
        }

        return fail(ln, "internal: unhandled key \"" + k + "\"");
    }

    bool run(const char *data, size_t size)
    {
        size_t pos = 0;
        int number = 1;
        bool after_eof = false;
        while (pos < size) {
            size_t end = pos;
            while (end < size && data[end] != '\n') end++;
            std::string raw(data + pos, end - pos);
            // FUN_004f6240 cuts the line at the first ';', '#' or '/'.
            size_t cut = raw.find_first_of(";#/");
            if (cut != std::string::npos) raw.resize(cut);
            std::vector<std::string> tokens = split_ws(raw);
            if (!tokens.empty()) {
                if (after_eof) {
                    cfg->content_after_eof.push_back(number);
                } else {
                    CfgLine ln;
                    ln.number = number;
                    ln.key = tokens[0];
                    ln.values.assign(tokens.begin() + 1, tokens.end());
                    ln.known = known_key(ln.key);
                    cfg->lines.push_back(ln);
                    if (ln.key == "eof") {
                        cfg->has_eof = true;
                        cfg->eof_line = number;
                        after_eof = true;
                    } else if (ln.known && !parse_known(ln)) {
                        return false;
                    }
                }
            }
            pos = end + 1;
            number++;
        }
        return true;
    }
};

} // namespace

bool cfg_parse(const void *data, size_t size, const char *source, CfgFile *out,
               std::string *err)
{
    if (!out) return false;
    *out = CfgFile();
    out->source = source ? source : "<memory>";
    if (err) err->clear();
    if (size != 0 && !data) {
        if (err) *err = out->source + ": null data";
        return false;
    }
    Parser p(out, source, err);
    return p.run((const char *)data, size);
}

bool cfg_load(const char *path, CfgFile *out, std::string *err)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) *err = std::string("cannot open ") + path;
        return false;
    }
    std::vector<char> data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        data.insert(data.end(), buf, buf + n);
    bool read_ok = !ferror(f);
    fclose(f);
    if (!read_ok) {
        if (err) *err = std::string("read error in ") + path;
        return false;
    }
    return cfg_parse(data.data(), data.size(), path, out, err);
}
