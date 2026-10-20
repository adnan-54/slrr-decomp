// rpkdump: inspect an RPAK package and its ImageCode .rdb companion, and run a
// strict self-check over the shipped sample pairs.
//
//   rpkdump FILE.rpk [--rdb FILE.rdb]        list entries with sizes
//   rpkdump FILE.rpk --entry 0xD2            dump one entry's data
//   rpkdump FILE.rpk --rdb FILE.rdb --resolve 0xD2
//                                            resource, class chain, properties, links
//   rpkdump FILE.rpk --rdb FILE.rdb --classes | --instances
//                                            list database classes / resources
//   rpkdump --selfcheck A.rpk A.rdb B.rpk B.rdb
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "resource/rdb.h"
#include "resource/rpk.h"

static void usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  rpkdump FILE.rpk [--rdb FILE.rdb]              list entries\n"
                 "  rpkdump FILE.rpk --entry ID                    dump one entry\n"
                 "  rpkdump FILE.rpk --rdb FILE.rdb --resolve ID   resolve id\n"
                 "  rpkdump FILE.rpk --rdb FILE.rdb --classes      list classes\n"
                 "  rpkdump FILE.rpk --rdb FILE.rdb --instances    list resources\n"
                 "  rpkdump --selfcheck A.rpk A.rdb [B.rpk B.rdb ...]\n");
}

static bool parse_id(const char *s, uint32_t *out)
{
    if (!s || !*s || *s == '-') return false;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s) return false;
    errno = 0;
    char *end = nullptr;
    unsigned long v = strtoul(s, &end, base);
    if (errno || end == s || *end != '\0' || v > 0xffffffffUL) return false;
    *out = (uint32_t)v;
    return true;
}

static std::string type_label(const Rdb *rdb, uint32_t type)
{
    if (!rdb) return std::string();
    return rdb->type_name(type);
}

static std::string normalize_package(std::string s)
{
    for (char &c : s) {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".rpk") == 0) s.resize(s.size() - 4);
    if (s.empty() || s.back() != '\\') s += '\\';
    return s;
}

static std::string external_label(uint32_t id, const RpkArchive &rpk)
{
    unsigned pkg = id >> 16, local = id & 0xffff;
    std::string name;
    if (pkg >= 1 && pkg <= rpk.subpackages.size())
        name = rpk.subpackages[pkg - 1].name;
    else
        name = "package " + std::to_string(pkg);
    char buf[16];
    snprintf(buf, sizeof buf, ":0x%04x", local);
    return name + buf;
}

static bool is_text(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == '\t' || c == '\r' || c == '\n') continue;
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static void hex_dump(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        std::printf("  %04zx  ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n)
                std::printf("%02x ", p[i + j]);
            else
                std::printf("   ");
        }
        std::printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = p[i + j];
            std::putchar(c >= 0x20 && c <= 0x7e ? c : '.');
        }
        std::printf("|\n");
    }
}

static void print_resource(const RdbResource &r)
{
    std::printf("  rdb: alias \"%s\" typeof %u super 0x%08x parentcompatible %.2f (%s)\n",
                r.alias.c_str(), (unsigned)r.type, (unsigned)r.super_id,
                r.parent_compatible, r.source_file.c_str());
}

static int list_entries(const RpkArchive &rpk, const Rdb *rdb)
{
    std::printf("rpk: version 0x%x, %zu record(s), %zu sub-package(s), "
                "table 0x%zx..0x%zx, data 0x%zx..0x%zx\n",
                (unsigned)rpk.version, rpk.records.size(), rpk.subpackages.size(),
                rpk.table_offset, rpk.table_end, rpk.table_end, rpk.size());
    for (size_t i = 0; i < rpk.subpackages.size(); i++)
        std::printf("  pkg[%zu] id %u %-40s tail 0x%x\n", i + 1,
                    (unsigned)rpk.subpackages[i].index,
                    ("\"" + rpk.subpackages[i].name + "\"").c_str(),
                    (unsigned)rpk.subpackages[i].tail);
    std::printf("  id        type %-22s flags       size  name\n", "");
    for (const RpkRecord &r : rpk.records) {
        std::printf("  %08x  %4u %-22s 0x%02x %10u  %s\n", (unsigned)r.id,
                    (unsigned)r.type, type_label(rdb, r.type).c_str(),
                    (unsigned)r.flags, (unsigned)r.data_size, r.name.c_str());
    }
    return 0;
}

static int dump_entry(const RpkArchive &rpk, const Rdb *rdb, uint32_t id)
{
    const RpkRecord *rec = rpk.find(id);
    if (!rec) {
        std::fprintf(stderr, "no entry 0x%08x\n", (unsigned)id);
        return 1;
    }
    std::printf("entry 0x%08x: \"%s\"\n", (unsigned)rec->id, rec->name.c_str());
    std::printf("  type %u", (unsigned)rec->type);
    std::string tn = type_label(rdb, rec->type);
    if (!tn.empty()) std::printf(" (%s)", tn.c_str());
    std::printf(" flags 0x%02x weight %.4f super 0x%08x\n", (unsigned)rec->flags,
                rec->weight, (unsigned)rec->super_id);
    std::printf("  data [0x%x, 0x%x) %u byte(s)%s\n", (unsigned)rec->data_offset,
                (unsigned)(rec->data_offset + rec->data_size),
                (unsigned)rec->data_size, rec->has_extra ? ", 48-byte extra block" : "");
    if (rdb) {
        const RdbResource *res = rdb->find(id);
        if (res)
            print_resource(*res);
        else
            std::printf("  rdb: not present in the database\n");
    }
    const uint8_t *p = rpk.data(*rec);
    if (rec->data_size == 0) {
        std::printf("  (empty)\n");
        return 0;
    }
    if (is_text(p, rec->data_size)) {
        std::fwrite(p, 1, rec->data_size, stdout);
        if (p[rec->data_size - 1] != '\n') std::putchar('\n');
    } else {
        hex_dump(p, rec->data_size);
    }
    return 0;
}

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Resource ids embedded in .rsd text: exactly eight hex digits after "0x".
static std::vector<uint32_t> scan_rsd_ids(const std::string &s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i + 10 <= s.size(); i++) {
        if (s[i] != '0' || (s[i + 1] != 'x' && s[i + 1] != 'X')) continue;
        if (i > 0 && (is_hex_digit(s[i - 1]) || s[i - 1] == '_')) continue;
        uint32_t v = 0;
        bool ok = true;
        for (int k = 0; k < 8; k++) {
            char c = s[i + 2 + k];
            if (!is_hex_digit(c)) {
                ok = false;
                break;
            }
            v = v * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        if (!ok) continue;
        if (i + 10 < s.size() && (is_hex_digit(s[i + 10]) || s[i + 10] == '_')) continue;
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    return out;
}

static void print_links(const RpkArchive &rpk, const Rdb &rdb, const std::string &rsd)
{
    std::vector<uint32_t> ids = scan_rsd_ids(rsd);
    if (ids.empty()) {
        std::printf("links: none\n");
        return;
    }
    std::printf("links:\n");
    for (uint32_t id : ids) {
        std::printf("  0x%08x -> ", (unsigned)id);
        const RdbResource *res = rdb.find(id);
        const RpkRecord *rec = rpk.find(id);
        if (id == 0)
            std::printf("(null/unset id)");
        else if (res)
            std::printf("\"%s\" (typeof %u)", res->alias.c_str(), (unsigned)res->type);
        else if (rec)
            std::printf("\"%s\" (type %u)", rec->name.c_str(), (unsigned)rec->type);
        else if (id >> 16)
            std::printf("%s (external)", external_label(id, rpk).c_str());
        else
            std::printf("(not in this pair)");
        std::printf("\n");
    }
}

static int resolve_id(const RpkArchive &rpk, const Rdb &rdb, uint32_t id)
{
    std::printf("resolve 0x%08x\n", (unsigned)id);
    const RpkRecord *rec = rpk.find(id);
    if (rec)
        std::printf("  rpk: \"%s\" type %u flags 0x%02x super 0x%08x data %u byte(s)\n",
                    rec->name.c_str(), (unsigned)rec->type, (unsigned)rec->flags,
                    (unsigned)rec->super_id, (unsigned)rec->data_size);
    else
        std::printf("  rpk: not present in this archive\n");
    const RdbResource *res = rdb.find(id);
    if (res)
        print_resource(*res);
    else
        std::printf("  rdb: not present in this database\n");

    std::printf("class chain:\n");
    std::vector<uint32_t> seen;
    uint32_t cur_id = id;
    bool stopped = false;
    for (int depth = 0; depth < 64; depth++) {
        if (cur_id == 0) {
            std::printf("  0x00000000 (root)\n");
            stopped = true;
            break;
        }
        const RdbResource *cur = rdb.find(cur_id);
        if (!cur) {
            if (cur_id >> 16)
                std::printf("  %s (external)\n", external_label(cur_id, rpk).c_str());
            else
                std::printf("  0x%08x (not in this database)\n", (unsigned)cur_id);
            stopped = true;
            break;
        }
        std::printf("  0x%08x  \"%s\"  type %u\n", (unsigned)cur->id,
                    cur->alias.c_str(), (unsigned)cur->type);
        if (cur->has_prop_count) {
            std::printf("    properties (%zu):\n", cur->props.size());
            for (const RdbProperty &prop : cur->props)
                std::printf("      %d %d %s\n", (int)prop.index, (int)prop.type,
                            prop.name.c_str());
            if (cur->prop_count != cur->props.size())
                std::printf("    warning: rpropcount %u but %zu rprop line(s)\n",
                            (unsigned)cur->prop_count, cur->props.size());
        }
        if (cur->super_id == 0) {
            std::printf("  0x00000000 (root)\n");
            stopped = true;
            break;
        }
        if (std::find(seen.begin(), seen.end(), cur->super_id) != seen.end()) {
            std::printf("  (super chain cycle)\n");
            stopped = true;
            break;
        }
        seen.push_back(cur->super_id);
        cur_id = cur->super_id;
    }
    if (!stopped) std::printf("  (chain truncated)\n");

    if (res && res->has_rsd) print_links(rpk, rdb, res->rsd);
    return 0;
}

static int list_classes(const Rdb &rdb)
{
    size_t n = 0;
    for (const RdbResource &r : rdb.resources)
        if (r.has_prop_count) n++;
    std::printf("classes: %zu property table(s)\n", n);
    for (const RdbResource &r : rdb.resources) {
        if (!r.has_prop_count) continue;
        std::printf("  %08x  %-28s type %-2u %4zu prop(s)%s\n", (unsigned)r.id,
                    r.alias.c_str(), (unsigned)r.type, r.props.size(),
                    r.prop_count == r.props.size() ? "" : "  [count mismatch]");
    }
    return 0;
}

static int list_instances(const Rdb &rdb)
{
    std::printf("resources: %zu\n", rdb.resources.size());
    for (const RdbResource &r : rdb.resources)
        std::printf("  %08x  %-28s type %-2u super 0x%08x\n", (unsigned)r.id,
                    r.alias.c_str(), (unsigned)r.type, (unsigned)r.super_id);
    return 0;
}

// ---- self-check ------------------------------------------------------------

struct Checker {
    int failures = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            failures++;
        }
    }
};

static bool check_door_part(const RpkArchive &rpk, const Rdb &rdb, Checker &c)
{
    const RpkRecord *rec = rpk.find(0xD2);
    const RdbResource *res = rdb.find(0xD2);
    c.check(rec != nullptr, "part 0x000000d2 exists in the archive");
    c.check(res != nullptr, "part 0x000000d2 exists in the database");
    if (!rec || !res) return false;
    c.check(rec->type == 8 && res->type == 8, "part is a game-object resource (type 8)");
    c.check(rec->name == res->alias, "archive name matches database alias");
    std::string data((const char *)rpk.data(*rec), rec->data_size);
    c.check(data.find("Axis_FL_door.class") != std::string::npos,
            "part data references Axis_FL_door.class");
    c.check(data.find("FL_door.cfg") != std::string::npos,
            "part data references FL_door.cfg");
    c.check(rdb.find(rec->super_id) != nullptr,
            "part super resource resolves in the database");

    // The part label is the cfg basename without extension, "_" -> " ".
    std::string label;
    size_t p = data.find("native part ");
    if (p != std::string::npos) {
        size_t begin = p + strlen("native part ");
        size_t end = data.find_first_of("\r\n", begin);
        std::string path = data.substr(begin, end == std::string::npos
                                                  ? std::string::npos
                                                  : end - begin);
        size_t slash = path.find_last_of("\\/");
        std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
        if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".cfg") == 0)
            base.resize(base.size() - 4);
        for (char &ch : base)
            if (ch == '_') ch = ' ';
        label = base;
    }
    c.check(label == "FL door",
            "part 0x000000d2 resolves to \"FL door\" (from FL_door.cfg)");
    if (res->alias != "FL door")
        std::printf("  note: database alias is \"%s\"; \"FL door\" is the part/cfg label\n",
                    res->alias.c_str());
    return label == "FL door";
}

static void selfcheck_pair(const char *rpk_path, const char *rdb_path, Checker &c,
                           bool *door_seen)
{
    std::printf("selfcheck %s + %s\n", rpk_path, rdb_path);
    RpkArchive rpk;
    std::string err;
    bool ok = rpk.load(rpk_path, &err);
    c.check(ok, "archive opens and parses" + (ok ? std::string() : ": " + err));
    if (!ok) return;
    Rdb rdb;
    ok = rdb.load(rdb_path, &err);
    c.check(ok, "database opens and parses" + (ok ? std::string() : ": " + err));
    if (!ok) return;

    c.check(rpk.record_count == rpk.records.size(),
            "record_count " + std::to_string(rpk.record_count) + " matches parsed entries");
    c.check(rpk.subpackages.size() == (size_t)rpk.count_a + rpk.count_b,
            "sub-package table matches count_a+count_b");
    c.check(rpk.table_offset + rpk.blob_size == rpk.table_end,
            "record table ends exactly at blob_size");
    c.check(rpk.table_end <= rpk.size(), "record table inside the file");
    c.check(rdb.has_external_links, "database has the external_links block");
    c.check(rdb.external_links.size() == rpk.subpackages.size(),
            "external link count matches sub-package count (" +
                std::to_string(rdb.external_links.size()) + ")");

    bool names_ok = true;
    for (size_t i = 0; i < rpk.subpackages.size() && i < rdb.external_links.size(); i++) {
        if (normalize_package(rpk.subpackages[i].name) !=
            normalize_package(rdb.external_links[i])) {
            names_ok = false;
            std::printf("  FAIL sub-package %zu: archive \"%s\" vs rdb \"%s\"\n", i + 1,
                        rpk.subpackages[i].name.c_str(), rdb.external_links[i].c_str());
        }
    }
    c.check(names_ok, "external link names match archive sub-package names");
    bool indexes_ok = true;
    for (size_t i = 0; i < rpk.subpackages.size(); i++)
        if (rpk.subpackages[i].index != i + 1) indexes_ok = false;
    c.check(indexes_ok, "sub-package indexes are 1..n in order");

    c.check(rdb.resources.size() == rpk.records.size(),
            "database resource count matches archive record count (" +
                std::to_string(rdb.resources.size()) + ")");
    size_t missing = 0, extra = 0, mism = 0, unnamed = 0;
    for (const RpkRecord &rec : rpk.records) {
        if (rec.name.empty()) unnamed++;
        const RdbResource *res = rdb.find(rec.id);
        if (!res) {
            missing++;
            continue;
        }
        if (res->type != rec.type || res->super_id != rec.super_id ||
            res->alias != rec.name)
            mism++;
    }
    for (const RdbResource &res : rdb.resources)
        if (!rpk.find(res.id)) extra++;
    c.check(missing == 0, "every archive entry has a database resource");
    c.check(extra == 0, "every database resource has an archive entry");
    c.check(mism == 0, "typeof/superid/alias match the archive records");
    c.check(unnamed == 0, "every archive entry has a name");

    size_t has_rsd = 0, text_match = 0, binary_skip = 0, rsd_bad = 0;
    for (const RpkRecord &rec : rpk.records) {
        const RdbResource *res = rdb.find(rec.id);
        if (!res) continue;
        if (res->has_rsd) has_rsd++;
        const uint8_t *p = rpk.data(rec);
        if (memchr(p, 0, rec.data_size)) {
            binary_skip++;
            continue;
        }
        std::string data((const char *)p, rec.data_size);
        if (!res->has_rsd || res->rsd != data) {
            rsd_bad++;
            if (rsd_bad <= 5)
                std::printf("  FAIL .rsd text differs for entry 0x%08x\n",
                            (unsigned)rec.id);
        } else {
            text_match++;
        }
    }
    c.check(has_rsd == rdb.resources.size(), "every database resource has an .rsd block");
    c.check(rsd_bad == 0, "text .rsd blocks equal archive data (" +
                              std::to_string(text_match) + " compared, " +
                              std::to_string(binary_skip) + " binary skipped)");

    size_t tables = 0, props = 0, bad_counts = 0;
    for (const RdbResource &res : rdb.resources) {
        if (!res.has_prop_count) continue;
        tables++;
        props += res.props.size();
        if (res.prop_count != res.props.size()) bad_counts++;
    }
    c.check(bad_counts == 0, "rpropcount equals the number of rprop lines");

    size_t oob = 0;
    for (const RpkRecord &rec : rpk.records)
        if ((uint64_t)rec.data_offset < rpk.table_end ||
            (uint64_t)rec.data_offset + rec.data_size > rpk.size())
            oob++;
    c.check(oob == 0, "every entry data range is inside the data area");

    std::vector<const RpkRecord *> ranges;
    for (const RpkRecord &rec : rpk.records)
        if (rec.data_size) ranges.push_back(&rec);
    std::sort(ranges.begin(), ranges.end(),
              [](const RpkRecord *a, const RpkRecord *b) {
                  return a->data_offset < b->data_offset;
              });
    bool contiguous = !ranges.empty() && ranges.front()->data_offset == rpk.table_end;
    for (size_t i = 1; i < ranges.size(); i++)
        if (ranges[i - 1]->data_offset + ranges[i - 1]->data_size !=
            ranges[i]->data_offset)
            contiguous = false;
    if (!ranges.empty() &&
        (uint64_t)ranges.back()->data_offset + ranges.back()->data_size != rpk.size())
        contiguous = false;
    c.check(contiguous, "data area is contiguous from the record table to EOF (" +
                            std::to_string(ranges.size()) + " ranges)");

    if (rpk.records.size() == 33) {
        c.check(rpk.subpackages.empty(), "system profile: no sub-packages");
        c.check(tables == 15, "system profile: 15 class property tables");
        c.check(props == 182, "system profile: 182 properties");
    }
    if (rpk.records.size() == 340) {
        c.check(rpk.subpackages.size() == 15, "Axis profile: 15 sub-packages");
        c.check(tables == 0, "Axis profile: no class property tables");
        static const unsigned want[21] = {0, 0, 0, 0, 0, 60, 1, 33, 80, 104,
                                          0, 0, 0, 0, 62, 0, 0, 0, 0, 0, 0};
        unsigned hist[21] = {};
        bool hist_ok = true;
        for (const RpkRecord &rec : rpk.records) {
            if (rec.type >= 21) {
                hist_ok = false;
                continue;
            }
            hist[rec.type]++;
        }
        for (int i = 0; i < 21; i++)
            if (hist[i] != want[i]) hist_ok = false;
        c.check(hist_ok, "Axis profile: type histogram 5:60 6:1 7:33 8:80 9:104 14:62");
    }

    if (rpk.find(0xD2)) {
        *door_seen = true;
        check_door_part(rpk, rdb, c);
    }
}

static int selfcheck(int argc, char **argv)
{
    if (argc < 2 || argc % 2 != 0) {
        std::fprintf(stderr, "usage: rpkdump --selfcheck A.rpk A.rdb [B.rpk B.rdb ...]\n");
        return 2;
    }
    Checker c;
    bool door_seen = false;
    for (int i = 0; i < argc; i += 2)
        selfcheck_pair(argv[i], argv[i + 1], c, &door_seen);
    c.check(door_seen, "Axis pair with part 0x000000d2 was among the checked pairs");
    std::printf("selfcheck: %s (%d failure(s))\n", c.failures ? "FAILED" : "PASSED",
                c.failures);
    if (c.failures) std::fprintf(stderr, "selfcheck FAILED (%d failure(s))\n", c.failures);
    return c.failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "--selfcheck") == 0) return selfcheck(argc - 2, argv + 2);

    const char *rpk_path = nullptr;
    const char *rdb_path = nullptr;
    const char *entry_arg = nullptr;
    const char *resolve_arg = nullptr;
    bool classes = false, instances = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        auto value = [&](const char *opt) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", opt);
                return nullptr;
            }
            return argv[++i];
        };
        if (strcmp(arg, "--rdb") == 0) {
            rdb_path = value(arg);
            if (!rdb_path) return 2;
        } else if (strcmp(arg, "--entry") == 0) {
            entry_arg = value(arg);
            if (!entry_arg) return 2;
        } else if (strcmp(arg, "--resolve") == 0) {
            resolve_arg = value(arg);
            if (!resolve_arg) return 2;
        } else if (strcmp(arg, "--classes") == 0) {
            classes = true;
        } else if (strcmp(arg, "--instances") == 0) {
            instances = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage();
            return 0;
        } else if (arg[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", arg);
            usage();
            return 2;
        } else if (!rpk_path) {
            rpk_path = arg;
        } else {
            std::fprintf(stderr, "unexpected argument %s\n", arg);
            return 2;
        }
    }
    if (!rpk_path) {
        usage();
        return 2;
    }
    if (classes && instances) {
        std::fprintf(stderr, "--classes and --instances are mutually exclusive\n");
        return 2;
    }
    if ((classes || instances || resolve_arg) && !rdb_path) {
        std::fprintf(stderr, "--classes/--instances/--resolve need --rdb FILE.rdb\n");
        return 2;
    }

    RpkArchive rpk;
    std::string err;
    if (!rpk.load(rpk_path, &err)) {
        std::fprintf(stderr, "%s: %s\n", rpk_path, err.c_str());
        return 1;
    }
    Rdb rdb;
    bool have_rdb = false;
    if (rdb_path) {
        if (!rdb.load(rdb_path, &err)) {
            std::fprintf(stderr, "%s: %s\n", rdb_path, err.c_str());
            return 1;
        }
        have_rdb = true;
    }

    if (classes) return list_classes(rdb);
    if (instances) return list_instances(rdb);
    if (entry_arg) {
        uint32_t id;
        if (!parse_id(entry_arg, &id)) {
            std::fprintf(stderr, "bad id '%s' (use decimal or 0xhex)\n", entry_arg);
            return 2;
        }
        return dump_entry(rpk, have_rdb ? &rdb : nullptr, id);
    }
    if (resolve_arg) {
        uint32_t id;
        if (!parse_id(resolve_arg, &id)) {
            std::fprintf(stderr, "bad id '%s' (use decimal or 0xhex)\n", resolve_arg);
            return 2;
        }
        return resolve_id(rpk, rdb, id);
    }
    return list_entries(rpk, have_rdb ? &rdb : nullptr);
}
