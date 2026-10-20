// ImageCode database reader (.rdb next to a .rpk; format: notes/01).
//
// The .rdb is a concatenation of text pseudo-files:
//   <FILE external_links >  one sub-package directory per line  </FILE>
//   <FILE 00000001.res >    typeof/superid/typeid/alias/isparentcompatible,
//                           optional rpropcount + rprop lines  </FILE>
//   <FILE 00000001.rsd >    resource data (the .rpk data area bytes)  </FILE>
// Files are read in order: each .rsd belongs to the most recent .res. Lines
// outside <FILE> blocks are comments and are ignored.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

struct RdbProperty {
    int32_t index = 0;
    int32_t type = 0;    // schema type code; negative values occur (-30, -126)
    std::string name;
};

struct RdbResource {
    uint32_t type = 0;                // typeof
    uint32_t super_id = 0;            // parent/class resource, pkg<<16 | local
    uint32_t id = 0;                  // typeid
    std::string alias;
    double parent_compatible = 0.0;   // isparentcompatible
    bool has_prop_count = false;
    uint32_t prop_count = 0;          // rpropcount as stored (may disagree with props)
    std::vector<RdbProperty> props;
    bool has_rsd = false;
    std::string rsd;                  // raw text of the matching .rsd block
    std::string source_file;          // <FILE ...> name, for diagnostics
};

class Rdb {
public:
    // Strict parse: every block must be terminated, every .res must carry the
    // four required fields, numbers must parse, and typeids must be unique.
    bool parse(const void *data, size_t size, std::string *err);
    bool load(const char *path, std::string *err);

    bool has_external_links = false;
    std::vector<std::string> external_links;   // as written, e.g. "cars\\racers\\"
    std::vector<RdbResource> resources;

    const RdbResource *find(uint32_t id) const;
    const RdbResource *find_alias(const std::string &alias) const;
    // Alias of the system record that names a type code (typ_/ins_/res_
    // resources), or "" when this database does not define it.
    std::string type_name(uint32_t type) const;
};
