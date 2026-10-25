// resman: package/class registry CLI and self-check.
//
//   resman [--resolve ID] RPK RDB [RPK RDB ...]
//       mount packages, load every referenced class/cfg, list classes; with
//       --resolve print the record's super-id chain, data references, class
//       units and cfg summary.
//   resman --selfcheck [GAME_DIR]
//       assert-based check over GAME_DIR/system.rpk+.rdb and
//       GAME_DIR/cars/racers/Axis.rpk+.rdb (default "bin/the game").
//   resman --sounds [GAME_DIR]
//       parse every res_sound record (type 6) in every paired .rpk under
//       GAME_DIR, cross-check with the .rdb .rsd text, and parse/decode every
//       WAV they reference (default "bin/the game").
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "audio/sound.h"
#include "game/pedestrians.h"
#include "game/runtime.h"
#include "game/traffic.h"
#include "platform/fs.h"
#include "resource/manager.h"
#include "script/vm.h"

static void usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  resman [--resolve ID] RPK RDB [RPK RDB ...]\n"
                 "  resman --selfcheck [GAME_DIR]\n"
                 "  resman --sounds [GAME_DIR]\n");
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

static std::string norm_ref(std::string s)
{
    for (char &c : s) {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

static bool ends_with_norm(const std::string &path, const char *suffix)
{
    std::string p = norm_ref(path);
    std::string s = norm_ref(suffix);
    return p.size() >= s.size() && p.compare(p.size() - s.size(), s.size(), s) == 0;
}

static void print_class_entry(const ClassEntry &e)
{
    std::printf("  %s\n", e.name.c_str());
    std::printf("    file     %s\n", e.path.c_str());
    if (!e.super_name.empty())
        std::printf("    extends  %s%s\n", e.super_name.c_str(),
                    e.super ? "" : " (external)");
    if (!e.interface_names.empty()) {
        std::string names;
        for (size_t i = 0; i < e.interface_names.size(); i++) {
            if (i) names += ", ";
            names += e.interface_names[i];
        }
        std::printf("    impl     %s\n", names.c_str());
    }
    const ClassUnit &u = *e.unit;
    std::printf("    unit     %zu constants, %zu fields, %zu methods, %zu bodies\n",
                u.consts.size(), u.fields.size(), u.methods.size(), u.bodies.size());
}

static void print_cfg_summary(const CfgFile &cfg)
{
    std::printf("  cfg      %s\n", cfg.source.c_str());
    std::printf("    %zu lines, eof=%s\n", cfg.lines.size(), cfg.has_eof ? "yes" : "no");
    std::printf("    render %zu, mesh %zu, texture %zu, click %zu, body %zu, "
                "slot %zu, stockpart %zu\n",
                cfg.renders.size(), cfg.meshes.size(), cfg.textures.size(),
                cfg.clicks.size(), cfg.bodies.size(), cfg.slots.size(),
                cfg.stockparts.size());
}

static int resolve_id(ResourceManager &mgr, uint32_t id)
{
    std::vector<ResourceHit> hits = mgr.lookup(id);
    if (hits.empty()) {
        std::printf("\n0x%08x: not found in the open packages\n", id);
        return 1;
    }
    int failures = 0;
    for (const ResourceHit &hit : hits) {
        const Package &pkg = *hit.package;
        const RpkRecord &rec = *hit.record;
        const RdbResource *res = pkg.rdb.find(rec.id);
        std::string type_name = mgr.type_name(rec.type);

        std::printf("\n");
        if (id != rec.id)
            std::printf("0x%08x (local 0x%08x) \"%s\"\n", id, rec.id, rec.name.c_str());
        else
            std::printf("0x%08x \"%s\"\n", rec.id, rec.name.c_str());
        std::printf("  package  %s\n", pkg.rpk_path.c_str());
        std::printf("  type     %u%s%s\n", rec.type, type_name.empty() ? "" : " ",
                    type_name.c_str());
        if (res)
            std::printf("  database alias \"%s\", superid 0x%08x\n", res->alias.c_str(),
                        res->super_id);

        std::vector<ChainLink> chain;
        std::string err;
        if (!mgr.chain(pkg, rec, &chain, &err)) {
            std::printf("  chain    FAILED: %s\n", err.c_str());
            failures++;
        } else {
            std::printf("  chain:\n");
            for (const ChainLink &link : chain) {
                if (link.record) {
                    std::string tn = mgr.type_name(link.record->type);
                    std::printf("    0x%08x %-20s type %u%s%s\n", link.id,
                                link.record->name.c_str(), link.record->type,
                                tn.empty() ? "" : " ", tn.c_str());
                } else if (link.external) {
                    std::printf("    0x%08x <external package %s, not open>\n", link.id,
                                link.package_path.c_str());
                } else {
                    std::printf("    0x%08x <missing in %s>\n", link.id,
                                link.package_path.c_str());
                }
            }
        }

        ClassPath classes;
        CfgFile cfg;
        bool have_cfg = false;
        std::vector<DataRef> refs = mgr.refs(pkg, rec);
        std::printf("  references:\n");
        if (refs.empty()) std::printf("    (none)\n");
        for (const DataRef &ref : refs) {
            if (ref.kind == DataRef::Script) {
                if (!ref.file) {
                    std::printf("    script   %s (class name, no file)\n", ref.text.c_str());
                } else if (ref.path.empty()) {
                    std::printf("    script   %s (UNRESOLVED)\n", ref.text.c_str());
                    failures++;
                } else {
                    std::printf("    script   %s -> %s\n", ref.text.c_str(),
                                ref.path.c_str());
                    std::string e;
                    if (!mgr.load_class(ref, &classes, &e)) {
                        std::printf("      class parse FAILED: %s\n", e.c_str());
                        failures++;
                    }
                }
            } else {
                if (ref.text.empty()) {
                    std::printf("    native   %s\n", ref.native.c_str());
                } else if (ref.path.empty()) {
                    std::printf("    native   %s %s (UNRESOLVED)\n", ref.native.c_str(),
                                ref.text.c_str());
                    failures++;
                } else {
                    std::printf("    native   %s %s -> %s\n", ref.native.c_str(),
                                ref.text.c_str(), ref.path.c_str());
                    if (ref.is_cfg) {
                        std::string e;
                        if (!mgr.load_cfg(ref, &cfg, &e)) {
                            std::printf("      cfg parse FAILED: %s\n", e.c_str());
                            failures++;
                        } else {
                            have_cfg = true;
                        }
                    }
                }
            }
        }
        classes.link();
        if (!classes.entries().empty()) {
            std::printf("  classes:\n");
            for (const auto &kv : classes.entries()) print_class_entry(kv.second);
        }
        if (have_cfg) print_cfg_summary(cfg);
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- self-check

struct Checker {
    int failures = 0;
    int checks = 0;
    void check(bool ok, const std::string &what)
    {
        checks++;
        if (!ok) failures++;
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    }
};

static int selfcheck(const char *game_dir)
{
    std::string sys_rpk = std::string(game_dir) + "/system.rpk";
    std::string sys_rdb = std::string(game_dir) + "/system.rdb";
    std::string axis_rpk = std::string(game_dir) + "/cars/racers/Axis.rpk";
    std::string axis_rdb = std::string(game_dir) + "/cars/racers/Axis.rdb";
    std::printf("resman selfcheck: %s\n", game_dir);

    Checker c;
    ResourceManager mgr;
    std::string err;

    bool sys_ok = mgr.open(sys_rpk.c_str(), sys_rdb.c_str(), &err);
    c.check(sys_ok, "open " + sys_rpk + " + system.rdb" +
                        (sys_ok ? "" : ": " + err));
    err.clear();
    bool axis_ok = mgr.open(axis_rpk.c_str(), axis_rdb.c_str(), &err);
    c.check(axis_ok, "open " + axis_rpk + " + Axis.rdb" +
                         (axis_ok ? "" : ": " + err));
    if (!sys_ok || !axis_ok) {
        std::printf("selfcheck: FAILED (%d failure(s))\n", c.failures);
        return 1;
    }

    c.check(mgr.package_count() == 2, "two packages mounted");
    c.check(mgr.package(0).rpk.record_count == 33,
            "system.rpk has 33 records (got " +
                std::to_string(mgr.package(0).rpk.record_count) + ")");
    c.check(mgr.package(1).rpk.record_count == 340,
            "Axis.rpk has 340 records (got " +
                std::to_string(mgr.package(1).rpk.record_count) + ")");
    c.check(mgr.package(0).rdb.resources.size() == 33, "system.rdb has 33 resources");
    c.check(mgr.package(1).rdb.resources.size() == 340, "Axis.rdb has 340 resources");
    c.check(mgr.type_name(8) == "typ_game_object", "type 8 names typ_game_object");
    c.check(mgr.type_name(9) == "typ_physics_body", "type 9 names typ_physics_body");

    std::vector<ResourceHit> hits = mgr.lookup(0xD2);
    c.check(hits.size() == 1, "id 0x000000d2 resolves in one open package");
    if (hits.empty()) {
        std::printf("selfcheck: FAILED (%d failure(s))\n", c.failures);
        return 1;
    }
    const Package &pkg = *hits[0].package;
    const RpkRecord &rec = *hits[0].record;
    c.check(norm_ref(pkg.rpk_path).find("axis") != std::string::npos,
            "0x000000d2 is the Axis package record (got " + pkg.rpk_path + ")");
    c.check(rec.id == 0xD2 && rec.name == "FL" && rec.type == 8,
            "record is \"FL\", type 8 (got \"" + rec.name + "\", " +
                std::to_string(rec.type) + ")");
    const RdbResource *res = pkg.rdb.find(rec.id);
    c.check(res && res->alias == "FL" && res->super_id == 7,
            "Axis.rdb carries alias \"FL\" and superid 0x00000007");

    std::vector<ChainLink> chain;
    err.clear();
    bool chain_ok = mgr.chain(pkg, rec, &chain, &err);
    c.check(chain_ok, "super-id chain resolves" + (chain_ok ? "" : ": " + err));
    c.check(chain.size() == 3,
            "chain has 3 links (got " + std::to_string(chain.size()) + ")");
    if (chain_ok && chain.size() == 3) {
        c.check(chain[0].record == &rec && chain[0].id == 0xD2,
                "link 0 is 0x000000d2 in Axis");
        c.check(chain[1].record && chain[1].id == 7 && chain[1].record->name == ">Stock" &&
                    !chain[1].external,
                "link 1 is 0x00000007 \">Stock\" in Axis");
        c.check(chain[2].external && chain[2].record == nullptr &&
                    chain[2].id == 0x4f242 &&
                    norm_ref(chain[2].package_path) == "parts.rpk",
                "link 2 is external parts.rpk local 0xf242 (0x0004f242)");
    }

    std::vector<DataRef> refs = mgr.refs(pkg, rec);
    c.check(refs.size() == 2, "0x000000d2 has two data references (got " +
                                  std::to_string(refs.size()) + ")");
    const DataRef *script_ref = nullptr, *cfg_ref = nullptr;
    for (const DataRef &r : refs) {
        if (r.kind == DataRef::Script) script_ref = &r;
        else if (r.is_cfg) cfg_ref = &r;
    }
    c.check(script_ref && script_ref->file && script_ref->is_class &&
                ends_with_norm(script_ref->path,
                               "cars\\racers\\axis_data\\scripts\\axis_fl_door.class"),
            "script reference resolves to Axis_FL_door.class");
    c.check(cfg_ref && cfg_ref->file && cfg_ref->is_cfg &&
                ends_with_norm(cfg_ref->path,
                               "cars\\racers\\axis_data\\scripts\\fl_door.cfg"),
            "native part reference resolves to FL_door.cfg");

    ClassPath classes;
    err.clear();
    bool loaded_class = script_ref && mgr.load_class(*script_ref, &classes, &err);
    c.check(loaded_class, "Axis_FL_door.class parses" +
                              (loaded_class ? "" : ": " + err));
    ClassEntry *door = classes.find("java.game.cars.Axis_FL_door");
    c.check(door != nullptr, "classpath registers java.game.cars.Axis_FL_door");
    if (door) {
        c.check(door->super_name == "java.game.parts.bodypart.FrontDoor",
                "Axis_FL_door extends java.game.parts.bodypart.FrontDoor");
        bool has_init = false;
        for (const MethodInfo &m : door->unit->methods) {
            const char *name = door->unit->str(m.name);
            if (name && std::string(name) == "<init>") has_init = true;
        }
        c.check(has_init, "Axis_FL_door declares <init>");
    }

    CfgFile part;
    err.clear();
    bool loaded_cfg = cfg_ref && mgr.load_cfg(*cfg_ref, &part, &err);
    c.check(loaded_cfg, "FL_door.cfg parses" + (loaded_cfg ? "" : ": " + err));
    if (loaded_cfg) {
        c.check(part.renders.size() == 1 && part.meshes.size() == 1 &&
                    part.textures.size() == 1 && part.clicks.size() == 1,
                "FL_door.cfg has render/mesh/texture/click lines");
        c.check(part.bodies.size() == 1 && part.slots.size() == 3 &&
                    part.stockparts.size() == 1 && part.has_eof,
                "FL_door.cfg has 1 body, 3 slots, 1 stockpart, eof");
        if (part.bodies.size() == 1) {
            const CfgBody &body = part.bodies[0];
            c.check(body.mass == 36.0f && norm_ref(body.shape).find("_phys_fl_door.scx") !=
                                              std::string::npos,
                    "FL_door.cfg body is 36.0 kg with _phys_FL_door.scx");
        }
        if (!part.slots.empty())
            c.check(part.slots[0].attaches.size() == 3,
                    "FL_door.cfg slot 4 carries three attach lines");
    }

    LoadReport report;
    mgr.load_all(&classes, &report);
    c.check(report.failures == 0,
            "0 parse failures over " + std::to_string(report.references) +
                " file reference(s)" +
                (report.failures ? ": " + report.problems.back() : ""));
    c.check(report.references == 145, "145 class/cfg file references (got " +
                                          std::to_string(report.references) + ")");
    c.check(report.class_files == 71,
            "71 class files loaded (got " + std::to_string(report.class_files) + ")");
    c.check(report.class_units == 72,
            "72 class units registered (got " + std::to_string(report.class_units) + ")");
    c.check(report.cfg_files == 70,
            "70 cfg files parsed (got " + std::to_string(report.cfg_files) + ")");
    c.check(report.unresolved == 4, "4 .class refs have no compiled file (got " +
                                        std::to_string(report.unresolved) + ")");
    c.check(report.empty == 0, "no zero-byte placeholders among these refs");

    // Install data: these 4 refs ship only as <dir>\src\Name.java. The exe's
    // record loader (0x51fcc0 -> 0x55aca0 -> 0x55bec0) runs JVM::compileSource
    // 0x5598f0, which compiles the source when the .class is missing, its mtime
    // differs from the source, or its header is not TUFA v4 build 0x11ae1/0x24f9d.
    // SERVICE MISSING: src has no Java compiler, so these stay unresolved.
    size_t java_backed = 0;
    for (size_t i = 0; i < mgr.package_count(); i++) {
        const Package &p = mgr.package(i);
        for (const RpkRecord &r : p.rpk.records) {
            for (const DataRef &ref : mgr.refs(p, r)) {
                if (!ref.file || !ref.is_class || !ref.path.empty()) continue;
                std::string java = ref.text;
                java.replace(java.size() - 6, 6, ".java");
                if (!mgr.resolve_path(java).empty()) java_backed++;
            }
        }
    }
    c.check(java_backed == 4, "the 4 unresolved .class refs have .java sources (got " +
                                  std::to_string(java_backed) + ")");

    classes.link();
    c.check(classes.size() == 72, "classpath has 72 names (got " +
                                      std::to_string(classes.size()) + ")");
    c.check(classes.duplicates() == 0, "no duplicate class names");
    c.check(classes.internal_links() == 4,
            "4 superclasses resolve inside the package set (got " +
                std::to_string(classes.internal_links()) + ")");
    c.check(classes.missing_links() == 68,
            "68 superclasses are external to the opened packages (got " +
                std::to_string(classes.missing_links()) + ")");
    ClassEntry *controller = classes.find("java.io.Controller");
    ClassEntry *controlconfig = classes.find("java.io.controlconfig");
    c.check(controlconfig && controller && controlconfig->super == controller,
            "controlconfig links to Controller");
    ClassEntry *models = classes.find("java.game.cars.Axis_models");
    ClassEntry *axis200s = classes.find("java.game.cars.Axis_200S");
    c.check(models && axis200s && axis200s->super == models,
            "Axis_200S links to Axis_models");
    ClassEntry *door2 = classes.find("java.game.cars.Axis_FL_door");
    c.check(door2 && door2->super == nullptr &&
                door2->super_name == "java.game.parts.bodypart.FrontDoor",
            "FrontDoor stays external (expected-missing)");

    Vm vm;
    for (const ClassUnit *unit : classes.units()) vm.add_class(unit);
    size_t vm_classes = 0;
    for (const auto &kv : classes.entries())
        if (vm.find_class(kv.first)) vm_classes++;
    c.check(vm_classes == classes.size(),
            "VM registers all " + std::to_string(classes.size()) + " classpath names");
    ClassState *vm_door = vm.find_class("java.game.cars.Axis_FL_door");
    c.check(vm_door != nullptr, "VM finds java.game.cars.Axis_FL_door");
    if (vm_door) {
        std::vector<Value> args;
        args.push_back(Value::make_int(0xD2));
        ClassState *decl = nullptr;
        std::string e;
        const MethodInfo *m = vm.find_method(vm_door, "<init>", "", 1, args, &decl, &e);
        c.check(m != nullptr, "VM links <init>(I)" + (m ? std::string() : ": " + e));
    }

    // Shipped traffic graph (Traffic__load 0x41a000): the grammar must cover the
    // whole file, node ids must resolve and the lane x-index must hold.
    std::string trc_path = std::string(game_dir) + "/maps/city/ai/city.trc";
    std::vector<uint8_t> trc;
    err.clear();
    bool have_trc = fs::read_file(trc_path, &trc, &err);
    c.check(have_trc, "read " + trc_path + (have_trc ? "" : ": " + err));
    traffic::Graph graph;
    if (have_trc) {
        size_t consumed = 0;
        err.clear();
        bool trc_ok = graph.load(trc.data(), trc.size(), &consumed, &err);
        c.check(trc_ok, "city.trc loads" + (trc_ok ? "" : ": " + err));
        if (trc_ok) {
            c.check(consumed == trc.size(),
                    "city.trc grammar covers the whole file (got " +
                        std::to_string(consumed) + " of " + std::to_string(trc.size()) +
                        " bytes)");

            size_t nodes = graph.node_count();
            bool ids_ok = true;
            uint32_t bad_id = 0;
            auto id_ok = [&](uint32_t id) {
                if (id > nodes && ids_ok) {
                    ids_ok = false;
                    bad_id = id;
                }
            };
            for (const traffic::Cross &cross : graph.cross_eq)
                for (const traffic::CrossLink &link : cross.links) id_ok(link.node);
            for (const traffic::Cross &cross : graph.cross_light)
                for (const traffic::CrossLink &link : cross.links) id_ok(link.node);
            for (const traffic::Road &road : graph.roads)
                for (uint32_t id : road.node) id_ok(id);
            for (const traffic::Lane &lane : graph.lanes) {
                id_ok(lane.road);
                id_ok(lane.node14);
                for (uint32_t id : lane.node88) id_ok(id);
            }
            c.check(ids_ok, "every traffic node id is <= " + std::to_string(nodes) +
                                (ids_ok ? "" : " (first bad id " +
                                                   std::to_string(bad_id) + ")"));

            std::vector<int> seen(graph.lanes.size(), 0);
            bool lane_index_ok = true;
            for (const traffic::Lane &lane : graph.lanes) {
                if (lane.index_lane >= seen.size() || seen[lane.index_lane]++)
                    lane_index_ok = false;
            }
            c.check(lane_index_ok,
                    "lane index_lane is a permutation of 0.." +
                        std::to_string(graph.lanes.size() ? graph.lanes.size() - 1 : 0));

            bool min_sorted = true, run_max_ok = true;
            float run_max = 0;
            for (size_t i = 0; i < graph.lanes.size(); i++) {
                if (i && graph.lanes[i].index_min_x < graph.lanes[i - 1].index_min_x)
                    min_sorted = false;
                if (i == 0 || graph.lanes[i].index_max_x > run_max)
                    run_max = graph.lanes[i].index_max_x;
                if (graph.lanes[i].index_run_max_x != run_max) run_max_ok = false;
            }
            c.check(min_sorted, "lane index_min_x is non-decreasing");
            c.check(run_max_ok, "lane index_run_max_x is the running max of index_max_x");

            float road_length = 0;
            for (const traffic::Road &road : graph.roads) road_length += road.path.length;
            std::printf("traffic: %zu eq crosses, %zu light crosses, %zu roads, "
                        "%zu lanes, %.1f m of road\n",
                        graph.cross_eq.size(), graph.cross_light.size(),
                        graph.roads.size(), graph.lanes.size(), road_length);
        }
    }

    // Shipped walk areas and road objects (0x4d5600 / 0x41a3e0): their params
    // are in the map RPK; the .walk/.rest editor exports are never opened.
    std::string city_rpk = std::string(game_dir) + "/maps/city.rpk";
    err.clear();
    bool city_ok = mgr.open(city_rpk.c_str(), nullptr, &err);
    c.check(city_ok, "open " + city_rpk + (city_ok ? "" : ": " + err));
    if (city_ok) {
        const Package &city = mgr.package(mgr.package_count() - 1);
        ped::Manager peds;
        size_t walk_params = 0, walk_bad = 0, road_params = 0, road_bad = 0;
        size_t kinds[4] = {};
        std::string walk_why, road_why;
        // Same byte mapping as decode_text, applied to the 8-char prefix.
        auto text_prefix = [](const std::string &s) -> uint32_t {
            if (s.size() < 8) return 0;
            uint32_t v = 0;
            for (int j = 0; j < 4; j++)
                v |= (uint32_t)(uint8_t)(((s[2 * j] - 1) << 4) | (s[2 * j + 1] - 0x41))
                     << (8 * j);
            return v;
        };
        // 0x519940: the nodes are embedded type-1 child records, so walk the
        // instance trees instead of scanning the file data area.
        std::vector<RpkMapObject> objects;
        bool map_ok = true;
        std::string map_err;
        for (const RpkRecord &rec : city.rpk.records) {
            if (rec.type != 1) continue;
            std::string e;
            if (!city.rpk.map_objects(rec, &objects, &e)) {
                map_ok = false;
                map_err = e;
                break;
            }
        }
        c.check(map_ok,
                "every type-1 city.rpk record parses" + (map_ok ? "" : ": " + map_err));
        size_t road_extra_bad = 0, road_phys_bad = 0, road_child_bad = 0;
        size_t road_bind_bad = 0;
        std::string road_bind_why;
        std::set<std::string> road_values;
        for (const RpkMapObject &obj : objects) {
            std::string at = obj.rec.name;
            if (obj.gametype == 0x352) {
                walk_params++;
                int32_t index = 0;
                std::string text;
                if (!ped::split_walk_params(obj.params, &index, &text)) {
                    walk_bad++;
                    walk_why = at + ": no ';'";
                    continue;
                }
                std::vector<uint8_t> dec = ped::decode_text(text.c_str());
                ped::Area area;
                size_t used = 0;
                if (!ped::parse_area(dec.data(), dec.size(), &area, &used)) {
                    walk_bad++;
                    walk_why = at + ": parse failed at index " + std::to_string(index);
                    continue;
                }
                if (used != dec.size()) {
                    walk_bad++;
                    walk_why = at + ": used " + std::to_string(used) + " of " +
                               std::to_string(dec.size());
                    continue;
                }
                if (text_prefix(text) != dec.size()) {
                    walk_bad++;
                    walk_why = at + ": prefix " + std::to_string(text_prefix(text)) +
                               " vs " + std::to_string(dec.size());
                    continue;
                }
                peds.add_area(index, text.c_str());
            } else if (obj.gametype == 0x15) {
                road_params++;
                road_values.insert(obj.params);
                if (!obj.rec.has_extra) road_extra_bad++;
                if (obj.phys) road_phys_bad++;
                if (obj.children != 0) road_child_bad++;
                // 0x41a3e0: the object binds itself; rec.id stands in for the
                // instance id (the exe passes the created object).
                if (!graph.bind_object(obj.params, obj.rec.id)) {
                    road_bind_bad++;
                    road_bind_why = at + ": bind returned 0";
                }
                int32_t kind = 0, gv = 0, idx = 0;
                if (!ped::parse_road_params(obj.params, &kind, &gv, &idx)) {
                    road_bad++;
                    road_why = at + ": parse failed";
                    continue;
                }
                bool ok = gv == (int32_t)graph.version;
                if (!ok)
                    road_why = at + ": graph " + std::to_string(gv);
                // INVENTED: the exe takes kinds 1..3 and does not bound idx.
                size_t pool = 0;
                if (kind == 1) pool = graph.roads.size();
                else if (kind == 2) pool = graph.cross_eq.size();
                else if (kind == 3) pool = graph.cross_light.size();
                else ok = false;
                if (ok && (idx < 0 || (size_t)idx >= pool)) {
                    ok = false;
                    road_why = at + ": idx " + std::to_string(idx) + " outside kind " +
                               std::to_string(kind);
                }
                if (!ok) road_bad++;
                else kinds[kind]++;
            }
        }
        c.check(walk_params == 1799,
                "city.rpk has 1799 walk params (got " + std::to_string(walk_params) + ")");
        c.check(walk_bad == 0,
                "every walk param decodes and parses" + (walk_bad ? ": " + walk_why : ""));
        c.check(peds.areas.size() == 1799,
                "walk area indices are unique (got " + std::to_string(peds.areas.size()) +
                    " areas)");
        c.check(road_params == 755,
                "city.rpk has 755 road params (got " + std::to_string(road_params) + ")");
        c.check(road_bad == 0,
                "every road param has graph " + std::to_string(graph.version) +
                    " and an in-pool idx" + (road_bad ? ": " + road_why : ""));
        c.check(kinds[1] == 484 && kinds[2] == 53 && kinds[3] == 218,
                "road kinds 484/53/218 = city.trc pools (got " + std::to_string(kinds[1]) +
                    "/" + std::to_string(kinds[2]) + "/" + std::to_string(kinds[3]) + ")");
        c.check(road_bind_bad == 0,
                "bind_object returns a node for every road object" +
                    (road_bind_bad ? ": " + road_bind_why : ""));
        size_t unbound = 0;
        for (const traffic::Road &road : graph.roads)
            if (!road.object) unbound++;
        for (const traffic::Cross &cross : graph.cross_eq)
            if (!cross.object) unbound++;
        for (const traffic::Cross &cross : graph.cross_light)
            if (!cross.object) unbound++;
        c.check(unbound == 0,
                "every pool node is bound once, 484/53/218 (got " +
                    std::to_string(unbound) + " unbound)");
        c.check(road_extra_bad == 0,
                "every road object has the 48-byte bounds block (got " +
                    std::to_string(road_extra_bad) + " without)");
        c.check(road_phys_bad == 0,
                "no road object has PHYS faces (got " + std::to_string(road_phys_bad) + ")");
        c.check(road_child_bad == 0,
                "no road object has children (got " + std::to_string(road_child_bad) + ")");
        c.check(road_values.size() == 755,
                "the 755 road params strings are pairwise distinct (got " +
                    std::to_string(road_values.size()) + ")");
        std::printf("walk/road: %zu walk areas, %zu road objects (%zu/%zu/%zu)\n",
                    peds.areas.size(), road_params, kinds[1], kinds[2], kinds[3]);
    }

    size_t records = 0;
    for (size_t i = 0; i < mgr.package_count(); i++)
        records += mgr.package(i).rpk.record_count;
    std::printf("counts: packages=%zu records=%zu references=%zu class_files=%zu "
                "class_units=%zu cfg_files=%zu unresolved=%zu empty=%zu failures=%zu "
                "internal_links=%zu external_links=%zu\n",
                mgr.package_count(), records, report.references, report.class_files,
                report.class_units, report.cfg_files, report.unresolved, report.empty,
                report.failures, classes.internal_links(), classes.missing_links());
    // Global ids (notes/16, 0x518940 / 0x51e860 / 0x517b80): a runtime on a bare
    // system.rpk mounts sl.rpk by name, and both routes to a system record give
    // the same global id. Expected names/types are the shipped records.
    {
        ResourceManager gm;
        std::string ge;
        bool g_ok = gm.open((std::string(game_dir) + "/system.rpk").c_str(), nullptr, &ge);
        c.check(g_ok, "open system.rpk alone" + (g_ok ? "" : ": " + ge));
        if (g_ok) {
            Runtime grt(gm);
            const Package *sl = gm.registry_package(gm.registry_index("sl"));
            c.check(sl != nullptr, "registry mounts sl.rpk by name");
            uint32_t init_id = sl ? gm.global_id(*sl, 0x40) : 0;
            Runtime::Resource *init = init_id ? grt.resolve(init_id) : nullptr;
            c.check(init && init->name == "Init" && init->type == 8,
                    "sl:0x40 resolves to type-8 record \"Init\"");
            c.check(init && init->parent && init->parent->name == "typ_game_object",
                    "Init's super 0x00010001 (sl sub-package system.rpk) is typ_game_object");
            const Package *sys = gm.registry_package(gm.registry_index("system"));
            uint32_t cfg_a = sys ? gm.global_id(*sys, 0x47) : 0;
            uint32_t cfg_b = sl ? gm.global_id(*sl, 0x00010047) : 0;
            c.check(cfg_a != 0 && cfg_a == cfg_b,
                    "system:0x47 has one global id from either package");
            Runtime::Resource *cfg = cfg_a ? grt.resolve(cfg_a) : nullptr;
            c.check(cfg && cfg->name == "Config", "system:0x47 resolves to \"Config\"");
        }
    }

    std::printf("selfcheck: %s (%d check(s), %d failure(s))\n",
                c.failures ? "FAILED" : "PASSED", c.checks, c.failures);
    return c.failures ? 1 : 0;
}

// ---------------------------------------------------------------- sounds

static char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool iends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (lower_ascii(s[s.size() - n + i]) != lower_ascii(suffix[i])) return false;
    return true;
}

static void find_rpks(const std::string &dir, std::vector<std::string> *out)
{
    std::vector<std::string> names;
    if (!fs::list_dir(dir, &names)) return;
    for (const std::string &name : names) {
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        std::vector<std::string> sub;
        if (fs::list_dir(path, &sub))   // a directory when its listing succeeds
            find_rpks(path, out);
        else if (iends_with(name, ".rpk"))
            out->push_back(path);
    }
}

static std::string sibling_rdb(const std::string &rpk)
{
    std::string stem = rpk.substr(0, rpk.size() - 4);
    if (fs::exists(stem + ".rdb")) return stem + ".rdb";
    if (fs::exists(stem + ".RDB")) return stem + ".RDB";
    return std::string();
}

static bool same_params(const audio::SoundParams &a, const audio::SoundParams &b)
{
    return a.sourcefile == b.sourcefile && a.volume == b.volume &&
           a.mindist == b.mindist && a.maxdist == b.maxdist &&
           a.instances == b.instances && a.flags == b.flags;
}

static int sounds(const char *game_dir)
{
    std::printf("resman sounds: %s\n", game_dir);

    std::vector<std::string> rpks;
    find_rpks(game_dir, &rpks);

    size_t paired = 0, opened = 0, open_failures = 0, rdb_failures = 0;
    size_t records = 0, without_source = 0;
    size_t compared = 0, no_rsd = 0, mismatches = 0;
    size_t missing_files = 0, read_failures = 0;
    size_t wav_ok = 0, wav_failures = 0, decode_ok = 0, rejected = 0;
    std::map<uint16_t, size_t> rejected_bits;
    std::string first_mismatch, first_wav_failure;

    for (const std::string &rpk_path : rpks) {
        // The game reads only the .rpk; a sibling .rdb (modding text DB) is an
        // optional cross-check and may be hand-edited beyond our parser.
        std::string rdb_path = sibling_rdb(rpk_path);
        if (!rdb_path.empty()) paired++;

        ResourceManager mgr;
        std::string err;
        if (!rdb_path.empty() && !mgr.open(rpk_path.c_str(), rdb_path.c_str(), &err)) {
            rdb_failures++;
            err.clear();
        }
        if (mgr.package_count() == 0 && !mgr.open(rpk_path.c_str(), nullptr, &err)) {
            open_failures++;
            std::printf("  open FAILED %s: %s\n", rpk_path.c_str(), err.c_str());
            continue;
        }
        opened++;

        const Package &pkg = mgr.package(0);
        for (const RpkRecord &rec : pkg.rpk.records) {
            if (rec.type != 6 || rec.data_size == 0) continue;
            records++;

            audio::SoundParams p;
            bool have_src = audio::parse_sound_params(
                (const char *)pkg.rpk.data(rec), rec.data_size, &p);

            const RdbResource *res = pkg.rdb.find(rec.id);
            if (res && res->has_rsd) {
                compared++;
                audio::SoundParams rp;
                audio::parse_sound_params(res->rsd.data(), res->rsd.size(), &rp);
                if (!same_params(p, rp)) {
                    mismatches++;
                    if (first_mismatch.empty()) {
                        char id[16];
                        std::snprintf(id, sizeof id, "0x%08x", rec.id);
                        first_mismatch = rpk_path + " " + id + " \"" + rec.name + "\"";
                    }
                }
            } else {
                no_rsd++;
            }

            if (!have_src) {
                without_source++;
                continue;
            }
            std::string path = mgr.resolve_path(p.sourcefile);
            if (path.empty()) {
                missing_files++;
                continue;
            }
            std::vector<uint8_t> bytes;
            std::string e;
            if (!fs::read_file(path, &bytes, &e)) {
                read_failures++;
                if (first_wav_failure.empty()) first_wav_failure = path + ": " + e;
                continue;
            }
            audio::WavView w;
            int r = audio::parse_wav(bytes.data(), bytes.size(), &w);
            if (r != 0) {
                wav_failures++;
                if (first_wav_failure.empty()) {
                    char code[16];
                    std::snprintf(code, sizeof code, "%d", r);
                    first_wav_failure = path + ": parse_wav " + code;
                }
                continue;
            }
            wav_ok++;
            std::vector<int16_t> stereo;
            if (audio::decode_pcm(w, &stereo))
                decode_ok++;
            else {
                rejected++;
                rejected_bits[w.bits]++;
            }
        }
    }

    Checker c;
    c.check(mismatches == 0,
            "0 rpk/rdb sound parameter mismatches over " + std::to_string(compared) +
                " record(s)" + (first_mismatch.empty() ? "" : ", first " + first_mismatch));
    c.check(wav_failures == 0,
            "0 WAV parse failures over " + std::to_string(wav_ok + wav_failures) +
                " WAV(s)" + (first_wav_failure.empty() ? "" : ", first " + first_wav_failure));

    c.check(open_failures == 0, "every .rpk opens without its .rdb (" +
                                    std::to_string(open_failures) + " failure(s))");
    std::printf("packages: %zu .rpk, %zu with .rdb (%zu .rdb unparsable), %zu opened, %zu open failure(s)\n",
                rpks.size(), paired, rdb_failures, opened, open_failures);
    std::printf("sounds: %zu type-6 record(s) with data, %zu without sourcefile\n",
                records, without_source);
    std::printf("rsd: %zu compared with .rdb, %zu without .rdb .rsd, %zu mismatch(es)\n",
                compared, no_rsd, mismatches);
    std::printf("files: %zu missing, %zu unreadable\n", missing_files, read_failures);
    std::printf("wav: %zu parsed, %zu failure(s); decode: %zu ok, %zu rejected",
                wav_ok, wav_failures, decode_ok, rejected);
    if (!rejected_bits.empty()) {
        std::printf(" (");
        bool first = true;
        for (const auto &kv : rejected_bits) {
            std::printf("%s%u-bit", first ? "" : ", ", (unsigned)kv.first);
            first = false;
        }
        std::printf(")");
    }
    std::printf("\n");
    std::printf("sounds: %s (%d check(s), %d failure(s))\n",
                c.failures ? "FAILED" : "PASSED", c.checks, c.failures);
    return c.failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "--selfcheck") == 0)
        return selfcheck(argc >= 3 ? argv[2] : "bin/the game");
    if (strcmp(argv[1], "--sounds") == 0)
        return sounds(argc >= 3 ? argv[2] : "bin/the game");

    uint32_t resolve_target = 0;
    bool have_resolve = false;
    std::vector<const char *> paths;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--resolve") == 0) {
            if (i + 1 >= argc || !parse_id(argv[++i], &resolve_target)) {
                std::fprintf(stderr, "--resolve needs a hex or decimal id\n");
                return 2;
            }
            have_resolve = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage();
            return 0;
        } else if (arg[0] == '-' && arg[1]) {
            std::fprintf(stderr, "unknown option %s\n", arg);
            usage();
            return 2;
        } else {
            paths.push_back(arg);
        }
    }
    if (paths.empty() || paths.size() % 2 != 0) {
        usage();
        return 2;
    }

    ResourceManager mgr;
    std::string err;
    for (size_t i = 0; i < paths.size(); i += 2) {
        if (!mgr.open(paths[i], paths[i + 1], &err)) {
            std::fprintf(stderr, "%s: %s\n", paths[i], err.c_str());
            return 1;
        }
    }

    std::printf("packages:\n");
    size_t records = 0;
    for (size_t i = 0; i < mgr.package_count(); i++) {
        const Package &p = mgr.package(i);
        records += p.rpk.record_count;
        std::printf("  [%zu] %s + %s: %u records, %zu rdb resources\n", i,
                    p.rpk_path.c_str(), p.rdb_path.c_str(), p.rpk.record_count,
                    p.rdb.resources.size());
    }
    std::printf("total: %zu package(s), %zu records\n", mgr.package_count(), records);

    ClassPath classes;
    LoadReport report;
    mgr.load_all(&classes, &report);
    classes.link();
    std::printf("classes: %zu name(s) from %zu file(s), %zu internal link(s), "
                "%zu external/missing\n",
                classes.size(), report.class_files, classes.internal_links(),
                classes.missing_links());
    for (const auto &kv : classes.entries()) {
        const ClassEntry &e = kv.second;
        std::printf("  %s\n    file     %s\n", e.name.c_str(), e.path.c_str());
        if (!e.super_name.empty())
            std::printf("    extends  %s%s\n", e.super_name.c_str(),
                        e.super ? "" : " (external)");
    }
    std::printf("references: %zu class/cfg ref(s), %zu class file(s), %zu cfg file(s), "
                "%zu unresolved, %zu empty, %zu failure(s)\n",
                report.references, report.class_files, report.cfg_files,
                report.unresolved, report.empty, report.failures);
    for (const std::string &problem : report.problems)
        std::printf("  note: %s\n", problem.c_str());

    if (have_resolve) return resolve_id(mgr, resolve_target);
    return 0;
}
