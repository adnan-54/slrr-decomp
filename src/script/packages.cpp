// JVM package tree, ported from the exe (addresses in the header).
#include "script/packages.h"

#include <stdio.h>

#include <vector>

#include "platform/fs.h"
#include "resource/manager.h"
#include "script/compiler/decl.h"
#include "script/compiler/emit.h"
#include "script/vm.h"

namespace jvm {

struct PackageTree::DirEntry {
    std::string path;               // exe-style relative dir ("sl\\scripts\\game")
    std::vector<std::string> names; // class names, filled on the first lookup
    bool listed = false;
};

struct PackageTree::Node {
    std::string name;
    std::vector<std::unique_ptr<Node>> children;  // sub-package nodes
    std::vector<DirEntry> dirs;                   // dirs registered at this package
    Node *parent = nullptr;
};

namespace {

bool ieq(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

// "X.java" -> "X" for ext ".java"; "" unless the text from the LAST '.' is
// exactly ext (0x55fd90 uses 0x569370 = strrchr; the name ends at that dot).
std::string stem(const std::string &file, const char *ext)
{
    size_t dot = file.rfind('.');
    if (dot == std::string::npos) return std::string();
    if (file.compare(dot, std::string::npos, ext) != 0) return std::string();
    return file.substr(0, dot);
}

} // namespace

PackageTree::PackageTree(std::string game_dir) : game_dir_(std::move(game_dir))
{
    // 0x55f330(0, 0, "java"): the tree root is the "java" package.
    root_ = std::make_unique<Node>();
    root_->name = "java";
}

PackageTree::~PackageTree() = default;

// 0x55ccc0: exactly these roots, in this order.
void PackageTree::add_default_roots()
{
    static const struct {
        const char *dir, *dotted;
    } kRoots[] = {
        {"system\\scripts\\lang", "java.lang"},
        {"system\\scripts\\net", "java.net"},
        {"system\\scripts\\io", "java.io"},
        {"system\\scripts\\util", "java.util"},
        {"system\\scripts\\render", "java.render"},
        {"system\\scripts\\sound", "java.sound"},
        {"sl\\scripts\\game", "java.game"},
        {"parts\\scripts", "java.game.parts"},
        {"parts\\accessories\\scripts", "java.game.parts.accessories"},
    };
    for (const auto &r : kRoots) add(r.dir, r.dotted);
}

int PackageTree::add(const std::string &dir, const std::string &dotted)
{
    return register_dir(root_.get(), dir, dotted);
}

// 0x55f330: {name, children, dirs, parent}; a non-null dir runs addDir.
std::unique_ptr<PackageTree::Node> PackageTree::make_node(Node *parent,
                                                          const std::string &dir,
                                                          const std::string &name)
{
    auto node = std::make_unique<Node>();
    node->name = name;
    node->parent = parent;
    if (!dir.empty()) add_dir(node.get(), dir);
    return node;
}

// 0x55f780 register(dir, dotted) on node N.
int PackageTree::register_dir(Node *node, const std::string &dir,
                              const std::string &dotted)
{
    size_t dot = dotted.find('.');
    if (dot != std::string::npos) {
        if (dotted.compare(0, dot, node->name) != 0) return 0;
        std::string rest = dotted.substr(dot + 1);
        for (const auto &child : node->children) {
            int r = register_dir(child.get(), dir, rest);
            if (r) return r;
        }
        size_t next = rest.find('.');
        // Intermediate chain node has no dir; the leaf carries it.
        node->children.push_back(next != std::string::npos
                                    ? make_node(node, std::string(), rest.substr(0, next))
                                    : make_node(node, dir, rest));
        if (next != std::string::npos)
            return register_dir(node->children.back().get(), dir, rest);
        return 1;
    }
    if (dotted != node->name) return 0;
    return add_dir(node, dir);
}

// 0x55fb50 (ECX = tree root via node+0xc): is dir registered at a node other
// than self? self's whole subtree is skipped.
bool PackageTree::dir_elsewhere(const Node *node, const Node *self, const std::string &dir)
{
    if (node == self) return false;
    for (const DirEntry &e : node->dirs)
        if (e.path == dir) return true;
    for (const auto &child : node->children)
        if (dir_elsewhere(child.get(), self, dir)) return true;
    return false;
}

// 0x55f550 addDir + 0x560180 subdirectory scan: a dir registered at another
// package returns -1 (0x55bec0 then logs "Package conflict"); a dir already on
// this node returns 1 without rescanning; otherwise the dir is added and every
// subdirectory (skip "." / "..") becomes a child node whose ctor recursively
// registers the whole subtree.
int PackageTree::add_dir(Node *node, const std::string &dir)
{
    if (dir_elsewhere(root_.get(), node, dir)) return -1;
    for (size_t i = node->dirs.size(); i-- > 0;)   // exe scans from the end
        if (node->dirs[i].path == dir) return 1;
    node->dirs.push_back(DirEntry{dir, {}, false});

    std::string disk = disk_path(dir);
    std::vector<fs::FindEntry> entries;
    if (!disk.empty() && fs::find(disk + "/*", &entries)) {
        for (const fs::FindEntry &e : entries) {
            if (!e.is_dir || e.name == "." || e.name == "..") continue;
            node->children.push_back(make_node(node, dir + "\\" + e.name, e.name));
        }
    }
    return 1;
}

// 0x55fd90: lazy listing on the first lookup; .java under src/ first, then
// class-only names from the dir. Case-sensitive membership test.
void PackageTree::list_names(DirEntry *entry)
{
    if (entry->listed) return;
    entry->listed = true;
    std::string disk = disk_path(entry->path);
    if (disk.empty()) return;

    std::vector<fs::FindEntry> files;
    // 0x55fd90: dir\src\*.java, case-insensitive like FindFirstFileA (SRC/Src).
    std::string src = disk_path(entry->path + "\\src");
    if (!src.empty() && fs::find(src + "/*.java", &files)) {
        for (const fs::FindEntry &f : files) {
            if (f.is_dir) continue;
            std::string name = stem(f.name, ".java");
            if (!name.empty()) entry->names.push_back(name);
        }
    }
    files.clear();
    if (fs::find(disk + "/*.class", &files)) {
        for (const fs::FindEntry &f : files) {
            if (f.is_dir) continue;
            std::string name = stem(f.name, ".class");
            if (name.empty()) continue;
            bool dup = false;
            for (const std::string &n : entry->names)
                if (n == name) {
                    dup = true;
                    break;
                }
            if (!dup) entry->names.push_back(name);
        }
    }
}

// 0x55fc10 findDir(fqn) on node N.
std::string PackageTree::find_dir(Node *node, const std::string &fqn)
{
    size_t dot = fqn.find('.');
    if (dot == std::string::npos) return std::string();
    if (fqn.compare(0, dot, node->name) != 0) return std::string();
    std::string rest = fqn.substr(dot + 1);
    if (rest.find('.') == std::string::npos) {
        for (DirEntry &e : node->dirs) {
            list_names(&e);
            for (const std::string &name : e.names)
                if (name == rest) return e.path;  // exact, case-sensitive strcmp
        }
        return std::string();
    }
    for (const auto &child : node->children) {
        std::string r = find_dir(child.get(), rest);
        if (!r.empty()) return r;
    }
    return std::string();
}

// 0x55c0c0 calls findDir on the root.
std::string PackageTree::find_dir(const std::string &fqn)
{
    return find_dir(root_.get(), fqn);
}

// INVENTED: Linux stand-in for case-insensitive Win32 path lookup. Walk from
// game_dir one segment at a time, matching each segment case-insensitively;
// "" when a segment is missing.
std::string PackageTree::disk_path(const std::string &exe_path) const
{
    if (exe_path.empty()) return std::string();
    std::string cur = game_dir_;
    size_t i = 0;
    for (;;) {
        size_t sep = exe_path.find_first_of("\\/", i);
        std::string seg = exe_path.substr(
            i, sep == std::string::npos ? std::string::npos : sep - i);
        if (!seg.empty()) {
            std::vector<std::string> names;
            fs::list_dir(cur, &names);
            std::string hit;
            for (const std::string &n : names)
                if (ieq(n, seg)) {
                    hit = n;
                    break;
                }
            if (hit.empty()) return std::string();
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += hit;
        }
        if (sep == std::string::npos) break;
        i = sep + 1;
    }
    return cur;
}

size_t PackageTree::count_nodes(const Node *node)
{
    size_t n = 1;
    for (const auto &child : node->children) n += count_nodes(child.get());
    return n;
}

size_t PackageTree::node_count() const
{
    return count_nodes(root_.get());
}

// 0x55c0c0 loadByName + 0x55bec0 load(dir, Name).
bool load_class(PackageTree &tree, ClassPath &classes, Vm &vm, const std::string &fqn)
{
    std::string dir = tree.find_dir(fqn);
    if (dir.empty()) {
        std::fprintf(stderr, "Class not found: %s\n", fqn.c_str());
        return false;
    }
    size_t dot = fqn.rfind('.');  // 0x569370 = strrchr
    std::string base = dot == std::string::npos ? fqn : fqn.substr(dot + 1);
    if (!load_class_at(tree, classes, vm, dir, base, nullptr)) return false;
    return vm.find_class(fqn) != nullptr;
}

// 0x5598f0 compileSource: <dir>\src\<base>.java stands in for a missing
// <dir>\<base>.class, which 0x55bec0 then reads back. src keeps the compiled
// bytes in memory instead of writing into the game dir (INVENTED, see below).
static bool compile_source(PackageTree &tree, ClassPath &classes, Vm &vm,
                           const std::string &dir, const std::string &base,
                           const std::string &rel, std::string *disk,
                           std::vector<uint8_t> *compiled)
{
    std::string java_rel = dir + "\\src\\" + base + ".java";
    std::string java = tree.disk_path(java_rel);   // case-insensitive: SRC/Src ok
    if (java.empty()) {
        std::fprintf(stderr, "JavaMachine::loadClass: failed to load %s\n", rel.c_str());
        return false;
    }
    std::string err;
    std::vector<uint8_t> src_bytes;
    if (!fs::read_file(java, &src_bytes, &err)) {
        std::fprintf(stderr, "JVM::compileSource: file not found: \"%s\"\n",
                     java_rel.c_str());
        return false;
    }
    std::string src(src_bytes.begin(), src_bytes.end());
    JavaUnit ju;
    std::vector<Token> tokens;
    if (!parse_java(src, &ju, &err, &tokens)) {
        // INVENTED: the exe quits the process (0x569570 -> _exit(-1)); src fails the load.
        std::fprintf(stderr, "JVM::compileSource: Syntax error in \"%s\" at line %s\n",
                     java_rel.c_str(), err.c_str());
        return false;
    }
    // 0x5557c0: the unit's package is registered with dir before codegen.
    tree.add(dir, ju.package.empty() ? std::string("java.unnamed") : ju.package);
    ClassIndex index;
    for (const ClassUnit *u : classes.units()) index.add(*u);
    // 0x55aa20: loaded classes, else JVM::getClass 0x55add0.
    index.load = [&](const std::string &name) -> const ClassUnit * {
        vm.get_class(name);
        ClassEntry *e = classes.find(name);
        return e ? e->unit : nullptr;
    };
    // shortcut: registry null (package spelling as written; runtime ids
    // resolve case-insensitively), upgrade when the registry keeps spellings.
    ClassFile out;
    if (!compile_unit(ju, tokens, index, &out, &err, nullptr)) {
        // INVENTED: the exe logs semantic errors and still writes the class.
        std::fprintf(stderr, "JVM::compileSource: %s: %s\n", java_rel.c_str(),
                     err.c_str());
        return false;
    }
    classfile_write(out, compiled);
    // INVENTED: the exe writes <dir>\<first class>.class and sets its time
    // to the source's (0x5557c0/0x5658d0); src keeps the bytes in memory so
    // a run never writes into the game dir.
    // 0x55bec0 reads <dir>\Name.class; a first class named otherwise would
    // not be read back.
    std::string first = out.units.empty()
                            ? std::string()
                            : out.units[0].class_name(out.units[0].self_class);
    size_t fdot = first.rfind('.');
    if (first.empty() ||
        first.substr(fdot == std::string::npos ? 0 : fdot + 1) != base) {
        std::fprintf(stderr, "JavaMachine::loadClass: failed to load %s\n", rel.c_str());
        return false;
    }
    std::string dir_disk = tree.disk_path(dir);
    *disk = dir_disk.empty() ? std::string() : dir_disk + "/" + base + ".class";
    if (disk->empty()) {
        std::fprintf(stderr, "JavaMachine::loadClass: failed to load %s\n", rel.c_str());
        return false;
    }
    return true;
}

// 0x55bec0 load(dir, Name): read <dir>\Name.class, add the new units to the
// VM and register the first one's package with dir.
bool load_class_at(PackageTree &tree, ClassPath &classes, Vm &vm, const std::string &dir,
                   const std::string &base, std::string *fqn_out)
{
    if (fqn_out) fqn_out->clear();
    std::string rel = dir + "\\" + base + ".class";
    std::string disk = tree.disk_path(rel);
    // 0x5598f0 compileSource: the exe compiles <dir>\src\Name.java when the
    // .class is missing.
    // shortcut: only the missing-.class case; the stale-time and bad-header
    // recompiles (1126 + 166 shipped pairs) wait for the remaining body diffs.
    bool from_source = false;
    std::vector<uint8_t> compiled;
    if (disk.empty()) {
        if (!compile_source(tree, classes, vm, dir, base, rel, &disk, &compiled))
            return false;
        from_source = true;
    }

    // Parse all units in the file; only the new ones go into the VM. Taken after
    // the compile: resolving its references may already have loaded classes.
    size_t before = classes.units().size();
    std::string err;
    bool added = from_source ? classes.add_data(disk, compiled, &err)
                             : classes.add_file(disk, &err);
    if (!added) {
        std::fprintf(stderr, "JavaMachine::loadClass: failed to load %s: %s\n", rel.c_str(),
                     err.c_str());
        return false;
    }
    for (size_t i = before; i < classes.units().size(); i++)
        vm.add_class(classes.units()[i]);

    // Register the class's package with its directory (0x55f780). A file whose
    // units were all already in the ClassPath has nothing new to register.
    if (classes.units().size() == before) return true;
    std::string cls_name = classes.units()[before]->class_name(
        classes.units()[before]->self_class);
    if (fqn_out) *fqn_out = cls_name;
    size_t pdot = cls_name.rfind('.');
    std::string pkg = pdot == std::string::npos ? std::string() : cls_name.substr(0, pdot);
    if (tree.add(dir, pkg) < 1)
        std::fprintf(stderr,
                     "Package conflict: multiple packages placed at the same location (%s, %s)\n",
                     cls_name.c_str(), dir.c_str());
    return true;
}

// 0x55aca0 -> 0x55bec0 for a record's `script <dir>\<Name>.class` ref. The
// registry lookup at 0x55aca0 keys on "dir.Name", which no shipped class name
// matches, so 0x55bec0 always reloads the file: every record gets fresh units
// and its own ClassState. The ClassPath still learns the file once (so name
// lookups keep working); that first load reuses its units.
ClassState *load_class_by_path(PackageTree &tree, ClassPath &classes, Vm &vm,
                               const std::string &dir, const std::string &base)
{
    std::string rel = dir + "\\" + base + ".class";
    std::string disk = tree.disk_path(rel);
    bool from_source = false;
    std::vector<uint8_t> compiled;
    if (disk.empty()) {
        if (!compile_source(tree, classes, vm, dir, base, rel, &disk, &compiled))
            return nullptr;
        from_source = true;
    }

    size_t before = classes.units().size();
    std::string err;
    bool added = from_source ? classes.add_data(disk, compiled, &err)
                             : classes.add_file(disk, &err);
    if (!added) {
        std::fprintf(stderr, "JavaMachine::loadClass: failed to load %s: %s\n", rel.c_str(),
                     err.c_str());
        return nullptr;
    }
    ClassState *first = nullptr;
    const ClassUnit *first_unit = nullptr;
    for (size_t i = before; i < classes.units().size(); i++) {
        ClassState *cs = vm.add_class(classes.units()[i]);
        if (!first) {
            first = cs;
            first_unit = classes.units()[i];
        }
    }
    if (!first) {
        // Already registered by name (e.g. the other 20 maps' track_data):
        // parse the bytes again into a ClassFile the VM owns.
        std::vector<uint8_t> bytes;
        const std::vector<uint8_t> *data = &compiled;
        if (!from_source) {
            std::string read_err;
            if (!fs::read_file(disk, &bytes, &read_err)) {
                std::fprintf(stderr, "JavaMachine::loadClass: failed to load %s: %s\n",
                             rel.c_str(), read_err.c_str());
                return nullptr;
            }
            data = &bytes;
        }
        auto file = std::make_unique<ClassFile>();
        std::string parse_err;
        if (!classfile_parse(data->data(), data->size(), file.get(), &parse_err)) {
            std::fprintf(stderr, "JavaMachine::loadClass: failed to load %s: %s\n",
                         rel.c_str(), parse_err.c_str());
            return nullptr;
        }
        for (const ClassUnit &u : file->units) {
            ClassState *cs = vm.add_class(&u);
            if (!first) {
                first = cs;
                first_unit = &u;
            }
        }
        vm.keep_file(std::move(file));
    }
    if (!first) return nullptr;

    // Register the class's package with its directory (0x55f780).
    std::string cls_name = first_unit->class_name(first_unit->self_class);
    size_t pdot = cls_name.rfind('.');
    std::string pkg = pdot == std::string::npos ? std::string() : cls_name.substr(0, pdot);
    if (tree.add(dir, pkg) < 1)
        std::fprintf(stderr,
                     "Package conflict: multiple packages placed at the same location (%s, %s)\n",
                     cls_name.c_str(), dir.c_str());
    return first;
}

} // namespace jvm
