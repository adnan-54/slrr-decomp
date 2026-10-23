// Resource/class registry: mount .rpk packages (optional .rdb for tool cross-checks), resolve ids and super-id
// chains across packages (id = sub-package index << 16 | local, 0 = the
// referring package), parse the `script`/`native` references in a record's
// data, and load the referenced TUFA class units and car-part configs.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "parts/cfg.h"
#include "resource/rdb.h"
#include "resource/rpk.h"
#include "script/classfile.h"

// One mounted archive with its database companion. mount is the normalized
// (lower-case, backslash, no .rpk) path used to match sub-package references.
struct Package {
    std::string rpk_path;
    std::string rdb_path;
    std::string mount;
    RpkArchive rpk;
    Rdb rdb;
};

// A record data reference: a `script` class path/FQN, or a `native` handler
// with an optional file argument ("native part <cfg>").
struct DataRef {
    enum Kind { Script, Native };
    Kind kind = Script;
    std::string text;    // path/class token as written
    std::string native;  // handler name ("part", "car", "camera", ...)
    std::string path;    // resolved file, "" when unresolved or not a file
    bool file = false;
    bool is_class = false;
    bool is_cfg = false;
};

// One loaded class unit, keyed by fully-qualified name in ClassPath.
struct ClassEntry {
    std::string name;
    std::string path;
    const ClassUnit *unit = nullptr;
    std::string super_name;
    std::vector<std::string> interface_names;
    ClassEntry *super = nullptr;   // null: external (not loaded) or missing
    std::vector<ClassEntry *> interfaces;
};

class ClassPath {
  public:
    // Parses every unit in the file and registers it by class name. A path is
    // loaded once; a name already present is skipped and counted as a duplicate.
    bool add_file(const std::string &path, std::string *err);
    // Wires superclass/interface names to entries; unmatched names stay null.
    void link();

    ClassEntry *find(const std::string &name);
    const std::map<std::string, ClassEntry> &entries() const { return entries_; }
    const std::vector<const ClassUnit *> &units() const { return units_; }
    size_t size() const { return entries_.size(); }
    size_t internal_links() const { return internal_links_; }
    size_t missing_links() const { return missing_links_; }
    size_t duplicates() const { return duplicates_; }

  private:
    std::vector<std::unique_ptr<ClassFile>> files_;
    std::map<std::string, ClassEntry> entries_;
    std::vector<const ClassUnit *> units_;
    std::vector<std::string> paths_;
    size_t internal_links_ = 0, missing_links_ = 0, duplicates_ = 0;
};

struct ResourceHit {
    const Package *package = nullptr;
    const RpkRecord *record = nullptr;
};

// One node of a super-id chain. external means the node lives in a sub-package
// that is not open; package_path is then the path as written in the table.
struct ChainLink {
    uint32_t id = 0;
    const Package *package = nullptr;
    const RpkRecord *record = nullptr;
    const RdbResource *resource = nullptr;
    bool external = false;
    std::string package_path;
};

struct LoadReport {
    size_t references = 0;   // class/cfg file references seen
    size_t class_files = 0;
    size_t class_units = 0;  // registry size after loading
    size_t cfg_files = 0;
    size_t empty = 0;        // zero-byte placeholders, skipped
    size_t unresolved = 0;   // file reference with no file on disk
    size_t failures = 0;     // existing file that failed to parse
    std::vector<std::string> problems;
};

class ResourceManager {
  public:
    // Opens an .rpk. The game needs only the .rpk: aliases (record names), super
    // ids, type names and .rsd text all live in it. rdb_path (may be null) loads
    // the modding text DB alongside, for tool cross-checks only.
    bool open(const char *rpk_path, const char *rdb_path, std::string *err);

    size_t package_count() const { return packages_.size(); }
    const Package &package(size_t i) const { return packages_[i]; }

    // All open packages in which id resolves. The high 16 bits select the
    // referring package's sub-package table; 0 selects the package itself.
    std::vector<ResourceHit> lookup(uint32_t id) const;
    const Package *find_open_package(const std::string &subpackage) const;

    std::string type_name(uint32_t type) const;
    bool chain(const Package &pkg, const RpkRecord &rec,
               std::vector<ChainLink> *out, std::string *err) const;
    std::vector<DataRef> refs(const Package &pkg, const RpkRecord &rec) const;

    // Resolves a Windows-style relative path against each package's directory
    // and its ancestors, case-insensitively; "" when no file exists.
    std::string resolve_path(const std::string &rel) const;

    bool load_class(const DataRef &ref, ClassPath *classes, std::string *err) const;
    bool load_cfg(const DataRef &ref, CfgFile *cfg, std::string *err) const;
    // Every class/cfg file reference in all open packages. Missing files and
    // zero-byte placeholders are counted, not treated as failures.
    void load_all(ClassPath *classes, LoadReport *report) const;

  private:
    // deque: Package pointers are handed out and must survive a runtime mount.
    std::deque<Package> packages_;
};
