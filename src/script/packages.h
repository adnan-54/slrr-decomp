// JVM package tree: fully-qualified class name -> script directory.
// Exe: 0x55ccc0 (registers the 9 roots), 0x55f330 (node ctor), 0x55f780
// (register), 0x55f550 (addDir), 0x560180 (subdirectory scan), 0x55fd90 (lazy
// class listing), 0x55fc10 (findDir). See notes/03-script-vm.md.
//
// The tree is named after the package root ("java"); dirs keep the exe's
// relative Win32 paths ("system\\scripts\\lang"). Subdirectories of a
// registered dir become sub-packages automatically.
#pragma once
#include <memory>
#include <string>

namespace jvm {

class PackageTree {
  public:
    // game_dir: where relative exe paths are resolved (case-insensitive, like Win32).
    explicit PackageTree(std::string game_dir);
    ~PackageTree();
    PackageTree(const PackageTree &) = delete;
    PackageTree &operator=(const PackageTree &) = delete;

    void add_default_roots();                            // 0x55ccc0, the 9 roots in order
    int add(const std::string &dir, const std::string &dotted);  // 0x55f780
    std::string find_dir(const std::string &fqn);        // 0x55fc10, "" when not found
    std::string disk_path(const std::string &exe_path) const;    // INVENTED helper
    size_t node_count() const;

  private:
    struct Node;
    struct DirEntry;

    std::unique_ptr<Node> make_node(Node *parent, const std::string &dir,
                                    const std::string &name);        // 0x55f330
    int register_dir(Node *node, const std::string &dir,
                     const std::string &dotted);         // 0x55f780
    int add_dir(Node *node, const std::string &dir);     // 0x55f550/0x560180
    void list_names(DirEntry *entry);                    // 0x55fd90
    std::string find_dir(Node *node, const std::string &fqn);  // 0x55fc10
    static size_t count_nodes(const Node *node);
    static bool dir_elsewhere(const Node *node, const Node *self,
                              const std::string &dir);   // 0x55fb50

    std::unique_ptr<Node> root_;
    std::string game_dir_;
};

} // namespace jvm
