// Resource/class registry. See manager.h; formats in notes/01-rpk-rdb-format.md.
#include "resource/manager.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

bool fail(std::string *err, const std::string &msg)
{
    if (err) *err = msg;
    return false;
}

char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool iequals(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower_ascii(*a) != lower_ascii(*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

bool iends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (lower_ascii(s[s.size() - n + i]) != lower_ascii(suffix[i])) return false;
    return true;
}

std::string lower_slashes(std::string s)
{
    for (char &c : s) {
        if (c == '/') c = '\\';
        c = lower_ascii(c);
    }
    return s;
}

// Package key: lower-case, backslashes, ".rpk" stripped, trailing backslash.
std::string canonical_package(std::string s)
{
    s = lower_slashes(s);
    while (!s.empty() && s.back() == '\\') s.pop_back();
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".rpk") == 0) s.resize(s.size() - 4);
    if (s.empty()) return std::string();
    return s + "\\";
}

std::string dirname_of(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    if (s == std::string::npos) return ".";
    if (s == 0) return p.substr(0, 1);
    return p.substr(0, s);
}

std::vector<std::string> split_path(const std::string &rel)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur != ".") parts.push_back(cur);
    return parts;
}

// Case-insensitive lookup of parts below base, matching Windows path semantics.
bool resolve_ci(const std::string &base, const std::vector<std::string> &parts,
                std::string *out)
{
    std::string cur = base;
    for (const std::string &part : parts) {
        DIR *d = opendir(cur.empty() ? "." : cur.c_str());
        if (!d) return false;
        bool found = false;
        while (struct dirent *e = readdir(d)) {
            if (!iequals(e->d_name, part.c_str())) continue;
            if (cur.empty())
                cur = e->d_name;
            else if (cur.back() == '/')
                cur += e->d_name;
            else
                cur += std::string("/") + e->d_name;
            found = true;
            break;
        }
        closedir(d);
        if (!found) return false;
    }
    struct stat st;
    if (parts.empty() || stat(cur.c_str(), &st) != 0) return false;
    *out = cur;
    return true;
}

bool file_size_of(const std::string &path, size_t *out)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    *out = (size_t)st.st_size;
    return true;
}

bool is_file_ref(const std::string &s)
{
    for (char c : s)
        if (c == '/' || c == '\\') return true;
    return iends_with(s, ".class") || iends_with(s, ".cfg");
}

std::vector<std::string> split_ws(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
        size_t begin = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') i++;
        if (i > begin) out.push_back(s.substr(begin, i - begin));
    }
    return out;
}

bool seen_path(std::vector<std::string> *seen, const std::string &path)
{
    for (const std::string &s : *seen)
        if (s == path) return true;
    seen->push_back(path);
    return false;
}

} // namespace

bool ClassPath::add_file(const std::string &path, std::string *err)
{
    for (const std::string &p : paths_)
        if (p == path) return true;

    auto file = std::make_unique<ClassFile>();
    std::string parse_err;
    if (!classfile_load(path.c_str(), file.get(), &parse_err))
        return fail(err, path + ": " + parse_err);
    for (const ClassUnit &u : file->units) {
        if (u.class_name(u.self_class).empty())
            return fail(err, path + ": class unit without a name");
    }

    paths_.push_back(path);
    files_.push_back(std::move(file));
    for (const ClassUnit &u : files_.back()->units) {
        std::string name = u.class_name(u.self_class);
        if (entries_.count(name)) {
            duplicates_++;
            continue;
        }
        ClassEntry entry;
        entry.name = name;
        entry.path = path;
        entry.unit = &u;
        if (u.super_class >= 0) entry.super_name = u.class_name(u.super_class);
        for (int idx : u.interfaces) {
            std::string iface = u.class_name(idx);
            if (!iface.empty()) entry.interface_names.push_back(iface);
        }
        entries_.emplace(name, std::move(entry));
        units_.push_back(&u);
    }
    return true;
}

void ClassPath::link()
{
    internal_links_ = missing_links_ = 0;
    for (auto &kv : entries_) {
        ClassEntry &e = kv.second;
        e.super = nullptr;
        e.interfaces.clear();
        if (!e.super_name.empty()) {
            auto it = entries_.find(e.super_name);
            if (it != entries_.end()) {
                e.super = &it->second;
                internal_links_++;
            } else {
                missing_links_++;
            }
        }
        for (const std::string &name : e.interface_names) {
            auto it = entries_.find(name);
            if (it != entries_.end()) {
                e.interfaces.push_back(&it->second);
                internal_links_++;
            } else {
                missing_links_++;
            }
        }
    }
}

ClassEntry *ClassPath::find(const std::string &name)
{
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

bool ResourceManager::open(const char *rpk_path, const char *rdb_path, std::string *err)
{
    if (!rpk_path || !rdb_path)
        return fail(err, "open needs an .rpk and an .rdb path");
    Package pkg;
    if (!pkg.rpk.load(rpk_path, err)) return false;
    if (!pkg.rdb.load(rdb_path, err)) return false;
    pkg.rpk_path = rpk_path;
    pkg.rdb_path = rdb_path;
    pkg.mount = canonical_package(rpk_path);
    packages_.push_back(std::move(pkg));
    return true;
}

const Package *ResourceManager::find_open_package(const std::string &subpackage) const
{
    std::string ref = canonical_package(subpackage);
    if (ref.empty()) return nullptr;
    for (const Package &pkg : packages_) {
        const std::string &mount = pkg.mount;
        if (mount.size() < ref.size()) continue;
        if (mount.compare(mount.size() - ref.size(), ref.size(), ref) != 0) continue;
        // Suffix match must start at a path boundary: X\Axis\ vs Axis\.
        if (mount.size() != ref.size() && mount[mount.size() - ref.size() - 1] != '\\')
            continue;
        return &pkg;
    }
    return nullptr;
}

std::vector<ResourceHit> ResourceManager::lookup(uint32_t id) const
{
    std::vector<ResourceHit> hits;
    uint32_t pkg_index = id >> 16;
    uint32_t local = id & 0xffff;
    for (const Package &pkg : packages_) {
        const Package *target = nullptr;
        if (pkg_index == 0) {
            target = &pkg;
        } else if ((size_t)pkg_index <= pkg.rpk.subpackages.size()) {
            target = find_open_package(pkg.rpk.subpackages[pkg_index - 1].name);
        }
        if (!target) continue;
        const RpkRecord *rec = target->rpk.find(pkg_index == 0 ? id : local);
        if (!rec) continue;
        bool dup = false;
        for (const ResourceHit &h : hits)
            if (h.package == target && h.record == rec) dup = true;
        if (!dup) hits.push_back(ResourceHit{target, rec});
    }
    return hits;
}

std::string ResourceManager::type_name(uint32_t type) const
{
    for (const Package &pkg : packages_) {
        std::string name = pkg.rdb.type_name(type);
        if (!name.empty()) return name;
    }
    return std::string();
}

bool ResourceManager::chain(const Package &pkg, const RpkRecord &rec,
                            std::vector<ChainLink> *out, std::string *err) const
{
    out->clear();
    const Package *cur_pkg = &pkg;
    const RpkRecord *cur = &rec;
    uint32_t id = rec.id;
    for (int guard = 0; guard < 128; guard++) {
        ChainLink link;
        link.id = id;
        link.package = cur_pkg;
        link.record = cur;
        link.resource = cur_pkg->rdb.find(cur->id);
        out->push_back(link);

        uint32_t next = cur->super_id;
        if (next == 0) return true;

        uint32_t pkg_index = next >> 16;
        uint32_t local = next & 0xffff;
        std::string ref;
        const Package *target = nullptr;
        if (pkg_index == 0) {
            target = cur_pkg;
        } else if ((size_t)pkg_index <= cur_pkg->rpk.subpackages.size()) {
            ref = cur_pkg->rpk.subpackages[pkg_index - 1].name;
            target = find_open_package(ref);
        } else {
            ref = "<bad sub-package index>";
        }
        if (!target) {
            ChainLink ext;
            ext.id = next;
            ext.external = true;
            ext.package_path = ref.empty() ? cur_pkg->rpk_path : ref;
            out->push_back(ext);
            return true;
        }
        const RpkRecord *next_rec = target->rpk.find(local);
        if (!next_rec) {
            ChainLink miss;
            miss.id = next;
            miss.package = target;
            miss.external = target != cur_pkg;
            miss.package_path = miss.external ? ref
                                              : (ref.empty() ? target->rpk_path : ref);
            out->push_back(miss);
            return true;
        }
        cur_pkg = target;
        cur = next_rec;
        id = next;
    }
    return fail(err, "super-id chain does not terminate");
}

std::vector<DataRef> ResourceManager::refs(const Package &pkg, const RpkRecord &rec) const
{
    std::vector<DataRef> out;
    const char *data = (const char *)pkg.rpk.data(rec);
    size_t size = rec.data_size;
    size_t pos = 0;
    while (pos < size) {
        size_t end = pos;
        while (end < size && data[end] != '\n') end++;
        std::string line(data + pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> tokens = split_ws(line);
        if (!tokens.empty() && tokens[0] == "script" && tokens.size() >= 2) {
            DataRef ref;
            ref.kind = DataRef::Script;
            ref.text = tokens[1];
            ref.file = is_file_ref(ref.text);
            ref.is_class = iends_with(ref.text, ".class");
            ref.is_cfg = iends_with(ref.text, ".cfg");
            if (ref.file) ref.path = resolve_path(ref.text);
            out.push_back(std::move(ref));
        } else if (!tokens.empty() && tokens[0] == "native" && tokens.size() >= 2) {
            DataRef ref;
            ref.kind = DataRef::Native;
            ref.native = tokens[1];
            if (tokens.size() >= 3) {
                ref.text = tokens[2];
                ref.file = is_file_ref(ref.text);
                ref.is_class = iends_with(ref.text, ".class");
                ref.is_cfg = iends_with(ref.text, ".cfg");
                if (ref.file) ref.path = resolve_path(ref.text);
            }
            out.push_back(std::move(ref));
        }
        pos = end + 1;
    }
    return out;
}

std::string ResourceManager::resolve_path(const std::string &rel) const
{
    if (rel.empty()) return std::string();
    std::vector<std::string> parts = split_path(rel);
    if (parts.empty()) return std::string();

    std::string hit;
    bool absolute = rel[0] == '/' || rel[0] == '\\';
    if (absolute && resolve_ci("/", parts, &hit)) return hit;

    std::vector<std::string> tried;
    for (const Package &pkg : packages_) {
        std::string base = dirname_of(pkg.rpk_path);
        for (;;) {
            if (!seen_path(&tried, base) && resolve_ci(base, parts, &hit)) return hit;
            std::string parent = dirname_of(base);
            if (parent == base) break;
            base = parent;
        }
    }
    if (!seen_path(&tried, std::string()) && resolve_ci("", parts, &hit)) return hit;
    return std::string();
}

bool ResourceManager::load_class(const DataRef &ref, ClassPath *classes,
                                 std::string *err) const
{
    if (!classes) return fail(err, "no classpath");
    if (!ref.file || ref.path.empty())
        return fail(err, "no class file for \"" + ref.text + "\"");
    return classes->add_file(ref.path, err);
}

bool ResourceManager::load_cfg(const DataRef &ref, CfgFile *cfg, std::string *err) const
{
    if (!cfg) return fail(err, "no output cfg");
    if (!ref.file || ref.path.empty())
        return fail(err, "no cfg file for \"" + ref.text + "\"");
    return cfg_load(ref.path.c_str(), cfg, err);
}

void ResourceManager::load_all(ClassPath *classes, LoadReport *report) const
{
    if (!classes || !report) return;
    *report = LoadReport();
    std::vector<std::string> seen_classes, seen_cfgs;
    for (const Package &pkg : packages_) {
        for (const RpkRecord &rec : pkg.rpk.records) {
            for (const DataRef &ref : refs(pkg, rec)) {
                if (!ref.file || (!ref.is_class && !ref.is_cfg)) continue;
                report->references++;
                if (ref.path.empty()) {
                    report->unresolved++;
                    report->problems.push_back("unresolved: " + ref.text);
                    continue;
                }
                size_t size = 0;
                if (!file_size_of(ref.path, &size) || size == 0) {
                    report->empty++;
                    continue;
                }
                std::string err;
                if (ref.is_class) {
                    if (seen_path(&seen_classes, ref.path)) continue;
                    if (classes->add_file(ref.path, &err)) {
                        report->class_files++;
                    } else {
                        report->failures++;
                        report->problems.push_back(err);
                    }
                } else {
                    if (seen_path(&seen_cfgs, ref.path)) continue;
                    CfgFile cfg;
                    if (cfg_load(ref.path.c_str(), &cfg, &err)) {
                        report->cfg_files++;
                    } else {
                        report->failures++;
                        report->problems.push_back(err);
                    }
                }
            }
        }
    }
    report->class_units = classes->size();
}
