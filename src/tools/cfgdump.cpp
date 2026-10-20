// cfgdump: parse a car-part .cfg (render/mesh/body/slot/attach grammar) and
// print its lines with the typed part model, or self-check the shipped sample.
//
//   cfgdump FILE.cfg
//   cfgdump --selfcheck [FILE.cfg]
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "parts/cfg.h"

static void usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  cfgdump FILE.cfg          print lines and parsed part data\n"
                 "  cfgdump --selfcheck [FILE.cfg]\n"
                 "                            default: bin/the game/cars/racers/"
                 "Axis_data/scripts/FL_door.cfg\n");
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void print_xform(const CfgPosRot &x)
{
    std::printf("pos (%.4f %.4f %.4f) rot (%.4f %.4f %.4f)", x.px, x.py, x.pz,
                x.rx, x.ry, x.rz);
}

static void print_attaches(const char *name, const std::vector<CfgAttach> &list)
{
    for (const CfgAttach &a : list)
        std::printf("    %-10s 0x%08x -> slot %d\n", name, (unsigned)a.part,
                    (int)a.slot);
}

static void dump(const CfgFile &cfg)
{
    std::printf("cfg %s: %zu content line(s), eof %s", cfg.source.c_str(),
                cfg.lines.size(), cfg.has_eof ? "present" : "missing");
    if (cfg.has_eof) std::printf(" at line %d", cfg.eof_line);
    std::printf("\n");

    std::printf("lines:\n");
    for (const CfgLine &ln : cfg.lines) {
        std::printf("  %4d  %-10s", ln.number, ln.key.c_str());
        for (const std::string &v : ln.values) std::printf(" %s", v.c_str());
        if (!ln.known) std::printf("   (unknown key: ignored)");
        std::printf("\n");
    }
    if (!cfg.content_after_eof.empty()) {
        std::printf("  content after eof (ignored, line(s)):");
        for (int line : cfg.content_after_eof) std::printf(" %d", line);
        std::printf("\n");
    }

    std::printf("part:\n");
    for (const CfgRender &r : cfg.renders) {
        std::printf("  render   0x%08x", (unsigned)r.id);
        if (r.has_xform) {
            std::printf(" ");
            print_xform(r.xform);
        }
        std::printf("\n");
    }
    for (const CfgMesh &m : cfg.meshes)
        std::printf("  mesh     0x%08x\n", (unsigned)m.id);
    for (const CfgTexture &t : cfg.textures)
        std::printf("  texture  0x%08x\n", (unsigned)t.id);
    for (const CfgClick &c : cfg.clicks) {
        std::printf("  click    \"%s\"", c.target.c_str());
        if (c.has_resource) std::printf(" resource 0x%08x", (unsigned)c.resource);
        else std::printf(" primitive/mesh");
        if (c.has_xform) {
            std::printf(" ");
            print_xform(c.xform);
        }
        std::printf("\n");
    }
    for (const CfgBody &b : cfg.bodies) {
        if (b.resource_form) {
            std::printf("  body     resource 0x%08x\n", (unsigned)b.resource);
            continue;
        }
        std::printf("  body     ");
        print_xform(b.xform);
        std::printf(" mass %.4f shape \"%s\"", b.mass, b.shape.c_str());
        if (b.tail_count) {
            std::printf(" size");
            for (int i = 0; i < b.tail_count; i++) std::printf(" %.4f", b.tail[i]);
        }
        std::printf("\n");
    }
    if (cfg.has_category) std::printf("  category %d\n", (int)cfg.category);
    for (const CfgSlot &s : cfg.slots) {
        std::printf("  slot %d  ", (int)s.id);
        print_xform(s.xform);
        std::printf("\n");
        print_attaches("attach", s.attaches);
        print_attaches("compatible", s.compatibles);
    }
    print_attaches("attach", cfg.orphan_attaches);
    print_attaches("compatible", cfg.orphan_compatibles);
    for (const CfgStockPart &s : cfg.stockparts)
        std::printf("  stockpart 0x%08x\n", (unsigned)s.id);
}

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

static bool feq(float a, float b) { return fabsf(a - b) < 1e-6f; }

static size_t count_key(const CfgFile &cfg, const char *key)
{
    size_t n = 0;
    for (const CfgLine &ln : cfg.lines)
        if (ln.key == key) n++;
    return n;
}

static int selfcheck(int argc, char **argv)
{
    const char *path = nullptr;
    const char *fallback[] = {
        "bin/FL_door.cfg",
        "bin/the game/cars/racers/Axis_data/scripts/FL_door.cfg",
        "../bin/FL_door.cfg",
        "../bin/the game/cars/racers/Axis_data/scripts/FL_door.cfg",
    };
    if (argc > 1) {
        usage();
        return 2;
    }
    if (argc == 1) {
        path = argv[0];
    } else {
        for (const char *candidate : fallback) {
            if (file_exists(candidate)) {
                path = candidate;
                break;
            }
        }
        if (!path) {
            std::fprintf(stderr, "selfcheck: no sample cfg found; tried:\n");
            for (const char *candidate : fallback)
                std::fprintf(stderr, "  %s\n", candidate);
            return 1;
        }
    }

    std::printf("selfcheck %s\n", path);
    CfgFile cfg;
    std::string err;
    if (!cfg_load(path, &cfg, &err)) {
        std::fprintf(stderr, "selfcheck FAILED: %s\n", err.c_str());
        return 1;
    }
    dump(cfg);

    Checker c;
    c.check(cfg.has_eof, "eof terminates the file");
    c.check(cfg.content_after_eof.empty(), "nothing but comments after eof");
    c.check(cfg.lines.size() == 14, "14 content lines before/including eof");
    c.check(!cfg.lines.empty() && cfg.lines.back().key == "eof",
            "the last content line is eof");

    size_t unknown = 0;
    for (const CfgLine &ln : cfg.lines)
        if (!ln.known) unknown++;
    c.check(unknown == 0, "every key is in the exe part handler table");

    static const struct {
        const char *key;
        size_t count;
    } want_keys[] = {
        {"render", 1},   {"mesh", 1},  {"texture", 1}, {"click", 1},
        {"body", 1},     {"category", 1}, {"slot", 3},  {"attach", 3},
        {"stockpart", 1}, {"eof", 1},
    };
    for (const auto &w : want_keys)
        c.check(count_key(cfg, w.key) == w.count,
                std::string(w.key) + " appears " + std::to_string(w.count) + " time(s)");

    c.check(cfg.renders.size() == 1, "one render line in the typed model");
    c.check(cfg.meshes.size() == 1, "one mesh line in the typed model");
    c.check(cfg.textures.size() == 1, "one texture line in the typed model");
    c.check(cfg.clicks.size() == 1, "one click line in the typed model");
    c.check(cfg.bodies.size() == 1, "one body line in the typed model");
    c.check(cfg.has_category, "category line in the typed model");
    c.check(cfg.slots.size() == 3, "three slot lines in the typed model");
    c.check(cfg.stockparts.size() == 1, "one stockpart line in the typed model");

    size_t links = cfg.orphan_attaches.size() + cfg.orphan_compatibles.size();
    for (const CfgSlot &s : cfg.slots)
        links += s.attaches.size() + s.compatibles.size();
    size_t modeled = cfg.renders.size() + cfg.meshes.size() + cfg.textures.size() +
                     cfg.clicks.size() + cfg.bodies.size() +
                     (cfg.has_category ? 1 : 0) + cfg.slots.size() + links +
                     cfg.stockparts.size() + (cfg.has_eof ? 1 : 0);
    c.check(modeled == cfg.lines.size(),
            "every content line is represented in the typed model (" +
                std::to_string(modeled) + "/" + std::to_string(cfg.lines.size()) + ")");

    if (cfg.renders.size() == 1) {
        const CfgRender &r = cfg.renders[0];
        c.check(r.id == 0x59 && !r.has_xform,
                "render id 0x00000059, no optional transform");
    }
    if (cfg.meshes.size() == 1)
        c.check(cfg.meshes[0].id == 0x5A, "mesh id 0x0000005a");
    if (cfg.textures.size() == 1)
        c.check(cfg.textures[0].id == 0x00020025, "texture id 0x00020025");
    if (cfg.clicks.size() == 1) {
        const CfgClick &k = cfg.clicks[0];
        c.check(k.target == "0x000000FF" && k.has_resource && k.resource == 0xFF &&
                    !k.has_xform,
                "click 0x000000ff resolves as a resource id");
    }
    if (cfg.bodies.size() == 1) {
        const CfgBody &b = cfg.bodies[0];
        c.check(!b.resource_form && feq(b.xform.px, 0.003f) &&
                    feq(b.xform.py, 0.001f) && feq(b.xform.pz, 0.023f),
                "body position 0.003 0.001 0.023");
        c.check(feq(b.xform.rx, 0) && feq(b.xform.ry, 0) && feq(b.xform.rz, 0),
                "body rotation 0 0 0");
        c.check(feq(b.mass, 36.0f) && b.tail_count == 0,
                "body mass 36.0 kg, no size tail");
        c.check(b.shape == "cars\\racers\\Axis_data\\meshes\\_phys_FL_door.scx",
                "body shape path (the ;0x20 id comment is stripped)");
    }
    if (cfg.has_category)
        c.check(cfg.category == 2, "category 2");
    if (cfg.stockparts.size() == 1)
        c.check(cfg.stockparts[0].id == 0xDB, "stockpart id 0x000000db");

    c.check(cfg.orphan_attaches.empty() && cfg.orphan_compatibles.empty(),
            "no attach/compatible lines before the first slot");
    if (cfg.slots.size() == 3) {
        const CfgSlot &door = cfg.slots[0];
        c.check(door.id == 4 && feq(door.xform.px, -0.094f) &&
                    feq(door.xform.py, -0.010f) && feq(door.xform.pz, -0.545f),
                "slot 4 (FL_door) at -0.094 -0.010 -0.545");
        c.check(feq(door.xform.rx, 0) && feq(door.xform.ry, 0) &&
                    feq(door.xform.rz, 0),
                "slot 4 rotation 0 0 0");
        c.check(door.attaches.size() == 3 && door.compatibles.empty(),
                "slot 4 owns the three attach lines, no compatible lines");
        if (door.attaches.size() == 3) {
            const CfgAttach *a = door.attaches.data();
            c.check(a[0].part == 0x6 && a[0].slot == 4,
                    "attach 0x00000006 -> slot 4 (200S chassis)");
            c.check(a[1].part == 0x157 && a[1].slot == 4,
                    "attach 0x00000157 -> slot 4 (200XT chassis)");
            c.check(a[2].part == 0x156 && a[2].slot == 4,
                    "attach 0x00000156 -> slot 4 (ZX360 chassis)");
        }
        const CfgSlot &window = cfg.slots[1];
        c.check(window.id == 6 && feq(window.xform.px, 0.076f) &&
                    feq(window.xform.py, 0.266f) && feq(window.xform.pz, 0.075f) &&
                    window.attaches.empty(),
                "slot 6 (FL_window) at 0.076 0.266 0.075, no attaches");
        const CfgSlot &mirror = cfg.slots[2];
        c.check(mirror.id == 11 && feq(mirror.xform.px, 0.090f) &&
                    feq(mirror.xform.py, 0.267f) && feq(mirror.xform.pz, -0.347f) &&
                    mirror.attaches.empty(),
                "slot 11 (L_mirror) at 0.090 0.267 -0.347, no attaches");
    }

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
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    if (argv[1][0] == '-') {
        std::fprintf(stderr, "unknown option %s\n", argv[1]);
        usage();
        return 2;
    }
    if (argc != 2) {
        std::fprintf(stderr, "unexpected extra argument %s\n", argv[2]);
        usage();
        return 2;
    }

    CfgFile cfg;
    std::string err;
    if (!cfg_load(argv[1], &cfg, &err)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    dump(cfg);
    return 0;
}
