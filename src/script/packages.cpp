// JVM package tree, ported from the exe (addresses in the header).
#include "script/packages.h"

#include <vector>

#include "platform/fs.h"

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

// 0x55f550 addDir + 0x560180 subdirectory scan: a dir already present returns
// -1; otherwise every subdirectory (skip "." / "..") becomes a child node whose
// ctor recursively registers the whole subtree.
int PackageTree::add_dir(Node *node, const std::string &dir)
{
    for (const DirEntry &e : node->dirs)
        if (e.path == dir) return -1;
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
    if (fs::find(disk + "/src/*.java", &files)) {
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

} // namespace jvm
