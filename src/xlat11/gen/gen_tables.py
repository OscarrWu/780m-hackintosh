#!/usr/bin/env python3
"""
gen_tables.py - generate the GFX10.3 -> GFX11 render-state translation tables
for src/xlat11 from authoritative register databases.

This is the RDNA3 skeleton copy of src/xlat12/gen/gen_tables.py. The structure
(NAME-driven classification, Linux cross-check, refusal conditions, by-name
field repack) is deliberately identical; the dialects and the register-set
input are the only differences:

  * source dialect   GFX10.3 (Apple-authored PM4, same as xlat12)
  * target dialect   GFX11   (Mesa gfx11.json, NOT gfx12.json)
  * register set     gen/skeleton_registers.txt (stage-1 core draw/shader path)
                     instead of the Apple mtl-regrecs.txt / pm4census.txt
                     captures, which do not exist for RDNA3 yet.

AUTHORITATIVE SOURCES (named; the commit/version each was read at):
  GFX10.3 addresses + fields
    Mesa  src/amd/registers/gfx103.json   @ mesa  4519cc5645880023ed1c83417d33c4cad99e88ee
    Linux drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_offset.h
                                           @ linux v7.2
  GFX11 addresses + fields
    Mesa  src/amd/registers/gfx11.json    @ mesa  4519cc56
    Linux .../gc/gc_11_0_0_offset.h       @ linux v7.2

The generator REFUSES (exit 2, nothing written) on: a missing source; a seed
name absent from gfx103.json; an ambiguous gfx10.3 address; a malformed field
bit range. A Linux header that names a table register at a different address
than Mesa is a source contradiction: that register is emitted as UNKNOWN (the
translator refuses it) and listed in the provenance header, never guessed.

Registers whose gfx10.3 and gfx11 field lists share NO field name have no
established value mapping; they are also emitted as UNKNOWN, not as a repack.

Outputs (checked in): xlat11_tables.h, xlat11_repack.h, xlat11_provenance.h
Run:  python3 src/xlat11/gen/gen_tables.py [--check]
"""
import json, os, re, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

MESA_COMMIT = "4519cc5645880023ed1c83417d33c4cad99e88ee"
LINUX_VERSION = "v7.2"

DEF_MESA = os.path.join(REPO, "re/graphics/src/mesa/src/amd/registers")
DEF_LINUX = os.path.join(REPO, "re/m2/linux/drivers/gpu/drm/amd/include/asic_reg/gc")
DEF_NAMES = os.path.join(HERE, "skeleton_registers.txt")

CTX_BASE, SH_BASE, UCF_BASE = 0x28000, 0x0B000, 0x30000   # Mesa sid.h:18-23


def die(msg):
    sys.stderr.write("gen_tables: REFUSING - %s\n" % msg)
    sys.exit(2)


# ---------------------------------------------------------------- Mesa ----
class MesaDB:
    def __init__(self, path):
        if not os.path.isfile(path):
            die("missing Mesa register database %s" % path)
        self.path = path
        self.fname = os.path.basename(path)
        j = json.load(open(path))
        self.names = {}
        self.by_addr = {}
        for e in j["register_mappings"]:
            self.names.setdefault(e["name"], e)
            self.by_addr.setdefault(e["map"]["at"], []).append(e["name"])
        self.types = j["register_types"]
        # line index for citations
        self.type_line = {}
        self.map_line = {}
        lines = open(path).read().split("\n")
        in_maps = in_types = False
        for i, ln in enumerate(lines, 1):
            if ln.startswith(' "register_mappings"'):
                in_maps, in_types = True, False
                continue
            if ln.startswith(' "register_types"'):
                in_maps, in_types = False, True
                continue
            if in_types:
                m = re.match(r'^  "(\w+)": \{', ln)
                if m:
                    self.type_line.setdefault(m.group(1), i)
            elif in_maps:
                m = re.search(r'"name": "(\w+)"', ln)
                if m:
                    self.map_line.setdefault(m.group(1), i)

    def fields(self, name):
        ent = self.names.get(name)
        if ent is None:
            return None, None
        tr = ent.get("type_ref", name)
        t = self.types.get(tr)
        if t is None:
            return None, tr
        out = []
        for f in t["fields"]:
            b = f["bits"]
            if not (isinstance(b, list) and len(b) == 2 and 0 <= b[0] <= b[1] <= 31):
                die("malformed bit range for %s.%s in %s" % (tr, f.get("name"), self.fname))
            out.append((f["name"], b[0], b[1]))
        return out, tr

    def cite_map(self, name):
        ln = self.map_line.get(name)
        if ln is None:
            die("no register_mappings line for %s in %s" % (name, self.fname))
        return "%s:%d" % (self.fname, ln)

    def cite_type(self, tr):
        ln = self.type_line.get(tr)
        return "%s:%d" % (self.fname, ln) if ln else "%s:?" % self.fname


# --------------------------------------------------------------- Linux ----
def load_linux(gc_dir, fname, prefix):
    path = os.path.join(gc_dir, fname)
    if not os.path.isfile(path):
        die("missing Linux header %s" % path)
    d, line = {}, {}
    for i, ln in enumerate(open(path), 1):
        m = re.match(r"#define %s(\w+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)\s*$" % prefix, ln)
        if not m:
            continue
        nm, bi, v = m.group(1), m.group(2), int(m.group(3), 0)
        e = d.setdefault(nm, [None, None])
        if bi:
            e[1] = v
        else:
            e[0] = v
            line[nm] = i
    return d, line


def derive_bases(mesa, lin):
    """Derive the per-BASE_IDX segment base from names both sources carry; the
       majority value is used, and agreement is reported (no hardcoded bases)."""
    votes = {}
    for nm, (off, bi) in lin.items():
        if off is None or bi is None:
            continue
        ent = mesa.names.get(nm)
        if ent is None or ent["map"].get("to") != "mm":
            continue
        a = ent["map"]["at"]
        if a % 4:
            continue
        votes.setdefault(bi, {}).setdefault(a // 4 - off, 0)
        votes[bi][a // 4 - off] += 1
    bases, report = {}, {}
    for bi, vv in votes.items():
        base, n = max(vv.items(), key=lambda kv: kv[1])
        bases[bi] = base
        report[bi] = (base, n, sum(vv.values()))
    return bases, report


def linux_name(lin, name):
    """Linux spelling of a Mesa register name. Mesa strips the _UMD suffix when
       it imports kernel headers (mesa src/amd/registers/parse_kernel_headers.py:
       848-849), so prefer X_UMD."""
    return name + "_UMD" if (name + "_UMD") in lin else name


def linux_addr(lin, bases, name):
    e = lin.get(linux_name(lin, name))
    if not e or e[0] is None or e[1] not in bases:
        return None
    return (bases[e[1]] + e[0]) * 4


# ------------------------------------------------------ classification ----
LEGACY_VS_RE = re.compile(r"^SPI_SHADER_(PGM_(LO|HI|RSRC[1-4])_VS|LATE_ALLOC_VS|USER_DATA_VS_\d+)$")


def absent_reason(name):
    if LEGACY_VS_RE.match(name):
        return "legacy-VS slot: gfx11 is NGG-only (no VS stage registers)"
    if re.search(r"CMASK|FMASK|_DCC|CLEAR_WORD|COVERAGE_OUT|CB_DCC", name):
        return "CMASK/FMASK/DCC: a linear uncompressed color target has none"
    if re.search(r"HTILE|STENCIL_CLEAR|DEPTH_CLEAR", name):
        return "HTILE/depth-clear: no depth target in a first triangle"
    if re.search(r"STRMOUT", name):
        return "streamout: unused by a first triangle"
    if re.search(r"^VGT_GS(_MODE|_ONCHIP|_PER_VS|_VERT_ITEMSIZE|VS_RING)|ESGS_RING|GSVS_RING", name):
        return "legacy GS mode/ring: the NGG pipeline uses none"
    if re.search(r"STENCILREFMASK", name):
        return "stencil ref/mask: gfx11 splits these; no stencil in a first triangle"
    if re.search(r"VRS|FSR|DFSM|PRELOAD_CONTROL|VTX_CNT_EN|INDEX_PAYLOAD|OUT_DEALLOC|VERTEX_REUSE|GE_PC_ALLOC|COHER_START|PERFMON|RMI_L2", name):
        return "feature removed on gfx11 / unused for a minimal draw"
    return "no gfx11 register of this name; unused by a first triangle"


def classify(name, m10, m11):
    a = m10.names.get(name)
    if a is None:
        return None
    g10 = a["map"]["at"]
    fa, t10 = m10.fields(name)
    b = m11.names.get(name)
    if b is None:
        cls = "legacy_vs" if LEGACY_VS_RE.match(name) else "absent"
        return dict(name=name, g10=g10, g12=0, cls=cls, reason=absent_reason(name),
                    fa=fa, fb=None, t10=t10, t12=None,
                    cite="g10 %s, absent from gfx11.json" % m10.cite_map(name))
    g12 = b["map"]["at"]
    fb, t12 = m11.fields(name)
    # Repack only when BOTH generations list fields and the lists differ. If
    # either side has no field list (opaque address dwords), the dword is
    # carried verbatim, classified by address alone.
    if fa is not None and fb is not None and fa != fb:
        cls = "field_repack"
    else:
        cls = "identical" if g10 == g12 else "moved"
    return dict(name=name, g10=g10, g12=g12, cls=cls, reason="", fa=fa, fb=fb,
                t10=t10, t12=t12,
                cite="g10 %s, g11 %s" % (m10.cite_map(name), m11.cite_map(name)))


def emit_repack(r, m10, m11, pfx):
    name, fa, fb = r["name"], r["fa"], r["fb"]
    b10 = {n: (lo, hi) for (n, lo, hi) in fa}
    b12 = {n: (lo, hi) for (n, lo, hi) in fb}
    shared = [n for (n, _, _) in fb if n in b10]
    dropped = [n for (n, _, _) in fa if n not in b12]
    added = [n for (n, _, _) in fb if n not in b10]
    L = ["/* %s  gfx10.3 0x%05x -> gfx11 0x%05x" % (name, r["g10"], r["g12"]),
         " * fields: %s (type %s) -> %s (type %s)  @mesa %s"
         % (m10.cite_type(r["t10"]), r["t10"], m11.cite_type(r["t12"]), r["t12"], MESA_COMMIT[:8])]
    if dropped:
        L.append(" * dropped (gfx10.3 only): %s" % ", ".join(dropped))
    if added:
        L.append(" * new on gfx11 (left 0):  %s" % ", ".join(added))
    L.append(" */")
    L.append("static uint32_t %srepack_%s(uint32_t v)" % (pfx, name))
    L.append("{")
    L.append("    uint32_t o = 0u;")
    notes = []
    for n in shared:
        s0, s1 = b10[n]
        d0, d1 = b12[n]
        sw, dw = s1 - s0 + 1, d1 - d0 + 1
        mask = (1 << min(sw, dw)) - 1
        c = "%s [%d:%d]->[%d:%d]" % (n, s1, s0, d1, d0)
        if sw != dw:
            c += " WIDTH %d->%d (masked to dst)" % (sw, dw)
            notes.append("%s.%s width %d->%d" % (name, n, sw, dw))
        L.append("    o |= ((v >> %2d) & 0x%xu) << %2d; /* %s */" % (s0, mask, d0, c))
    L.append("    return o;")
    L.append("}")
    return "\n".join(L), notes, dropped


# ---------------------------------------------------------------- main ----
def read_seed(path):
    if not os.path.isfile(path):
        die("missing register seed list %s" % path)
    names = []
    for ln in open(path):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        names.append(ln)
    if not names:
        die("register seed list %s is empty" % path)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mesa", default=DEF_MESA)
    ap.add_argument("--linux", default=DEF_LINUX)
    ap.add_argument("--names", default=DEF_NAMES)
    ap.add_argument("--out", default=os.path.join(REPO, "src/xlat11"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    m10 = MesaDB(os.path.join(args.mesa, "gfx103.json"))
    m11 = MesaDB(os.path.join(args.mesa, "gfx11.json"))
    l10, l10line = load_linux(args.linux, "gc_10_3_0_offset.h", "mm")
    l11, l11line = load_linux(args.linux, "gc_11_0_0_offset.h", "reg")
    b10, rep10 = derive_bases(m10, l10)
    b11, rep11 = derive_bases(m11, l11)

    seed = read_seed(args.names)

    entries = {}

    def add(r):
        g10 = r["g10"]
        if g10 in entries:
            if entries[g10]["cls"] != r["cls"] or entries[g10]["name"] != r["name"]:
                die("ambiguous gfx10.3 address 0x%x: %s (%s) vs %s (%s)"
                    % (g10, entries[g10]["name"], entries[g10]["cls"], r["name"], r["cls"]))
            return
        entries[g10] = r

    for nm in seed:
        r = classify(nm, m10, m11)
        if r is None:
            die("seed register %s is not named by gfx103.json" % nm)
        add(r)

    # --- Linux cross-check. A register whose two sources CONTRADICT each other
    # is not guessed: it becomes UNKNOWN (the translator refuses it) and is
    # listed in the provenance header. ---
    lx = dict(g10_agree=0, g10_unnamed=0, g11_agree=0, g11_unnamed=0, contradictions=0)
    contradictions = []
    for a, r in entries.items():
        if r["cls"] == "unknown":
            continue
        why = None
        la = linux_addr(l10, b10, r["name"])
        if la is None:
            lx["g10_unnamed"] += 1
        elif la != r["g10"]:
            why = ("contradictory gfx10.3 sources: gfx103.json 0x%x vs gc_10_3_0_offset.h:%d 0x%x"
                   % (r["g10"], l10line[linux_name(l10, r["name"])], la))
        else:
            lx["g10_agree"] += 1
            r["cite"] += ", gc_10_3_0_offset.h:%d" % l10line[linux_name(l10, r["name"])]
        if why is None and r["g12"]:
            lb = linux_addr(l11, b11, r["name"])
            if lb is None:
                lx["g11_unnamed"] += 1
            elif lb != r["g12"]:
                why = ("contradictory gfx11 sources: gfx11.json 0x%x vs gc_11_0_0_offset.h:%d 0x%x"
                       % (r["g12"], l11line[linux_name(l11, r["name"])], lb))
            else:
                lx["g11_agree"] += 1
                r["cite"] += ", gc_11_0_0_offset.h:%d" % l11line[linux_name(l11, r["name"])]
        if why is not None:
            lx["contradictions"] += 1
            contradictions.append("%s 0x%05x: %s" % (r["name"], a, why))
            r.update(cls="unknown", g12=0, reason=why, fa=None, fb=None)

    # A field-changed register whose two field lists share NO field name has no
    # established value mapping in the JSON. It is refused as UNKNOWN and
    # listed, not guessed.
    noshared = []
    for a, r in sorted(entries.items()):
        if r["cls"] != "field_repack":
            continue
        n10 = set(x[0] for x in r["fa"])
        if not any(x[0] in n10 for x in r["fb"]):
            noshared.append("%s 0x%05x: gfx10.3 %s %s | gfx11 %s %s"
                            % (r["name"], a, m10.cite_type(r["t10"]), [x[0] for x in r["fa"]],
                               m11.cite_type(r["t12"]), [x[0] for x in r["fb"]]))
            r.update(cls="unknown", g12=0, fa=None, fb=None,
                     reason="field lists share no field name (%s vs %s): no established value mapping"
                            % (m10.cite_type(r["t10"]), m11.cite_type(r["t12"])))

    # NGG rule constants, from the source JSON
    vse = m10.names.get("VGT_SHADER_STAGES_EN")
    if vse is None:
        die("VGT_SHADER_STAGES_EN missing from gfx103.json")
    vf, vtr = m10.fields("VGT_SHADER_STAGES_EN")
    prim = [f for f in vf if f[0] == "PRIMGEN_EN"]
    if len(prim) != 1 or prim[0][1] != prim[0][2]:
        die("PRIMGEN_EN is not a single bit in gfx103.json")
    ngg = dict(addr=vse["map"]["at"], bit=prim[0][1], cite=m10.cite_type(vtr))

    counts = {k: 0 for k in ("identical", "moved", "field_repack", "absent", "legacy_vs", "unknown")}
    for r in entries.values():
        counts[r["cls"]] += 1

    summary = ["seed registers %d, entries %d" % (len(seed), len(entries))]
    for k, v in counts.items():
        summary.append("%-12s %d" % (k, v))
    summary.append("Linux bases gfx10.3 %s  gfx11 %s" %
                   ({k: "0x%x (%d/%d names)" % v for k, v in rep10.items()},
                    {k: "0x%x (%d/%d names)" % v for k, v in rep11.items()}))
    summary.append("Linux cross-check: gfx10.3 agree %(g10_agree)d, not named %(g10_unnamed)d; "
                   "gfx11 agree %(g11_agree)d, not named %(g11_unnamed)d; contradictions (refused) %(contradictions)d" % lx)
    summary += ["contradiction (refused): " + x for x in contradictions]
    summary += ["no shared field (refused): " + x for x in noshared]
    summary.append("NGG rule: VGT_SHADER_STAGES_EN g10 0x%x PRIMGEN_EN bit %d (%s)" % (ngg["addr"], ngg["bit"], ngg["cite"]))

    if args.check:
        print("\n".join(summary))
        print("unknown (refused) addresses:")
        for a in sorted(entries):
            if entries[a]["cls"] == "unknown":
                print("    0x%05x %s" % (a, entries[a]["reason"]))
        return

    write_headers(args.out, entries, summary, ngg, m10, m11)
    print("\n".join(summary))


def cstr(s):
    return s.replace("\\", "\\\\").replace('"', "'")


def write_headers(out, entries, summary, ngg, m10, m11):
    banner = "/* GENERATED by src/xlat11/gen/gen_tables.py - do not edit. Regenerate instead. */\n"
    with open(os.path.join(out, "xlat11_provenance.h"), "w") as f:
        f.write(banner + "#ifndef XLAT11_PROVENANCE_H\n#define XLAT11_PROVENANCE_H\n")
        f.write('#define XLAT11_SRC_MESA_COMMIT  "%s"\n' % MESA_COMMIT)
        f.write('#define XLAT11_SRC_LINUX_VERSION "%s"\n' % LINUX_VERSION)
        f.write("/*\n")
        for s in summary:
            f.write(" * %s\n" % s)
        f.write(" */\n#endif\n")
    with open(os.path.join(out, "xlat11_repack.h"), "w") as f:
        f.write(banner + "#ifndef XLAT11_REPACK_H\n#define XLAT11_REPACK_H\n#include <stdint.h>\n\n")
        f.write("/* One pure value function per field-changed register: each shared field is\n"
                " * copied BY NAME from its gfx10.3 bit range to its gfx11 bit range; gfx10.3-only\n"
                " * fields are dropped, gfx11-only fields are left 0. */\n")
        f.write("\n")
        for a in sorted(entries):
            r = entries[a]
            if r["cls"] != "field_repack":
                continue
            code, _, _ = emit_repack(r, m10, m11, "")
            f.write(code + "\n\n")
        f.write("#endif\n")
    with open(os.path.join(out, "xlat11_tables.h"), "w") as f:
        f.write(banner + "#ifndef XLAT11_TABLES_H\n#define XLAT11_TABLES_H\n#include <stdint.h>\n")
        f.write('#include "xlat11.h"\n#include "xlat11_repack.h"\n\n/*\n')
        for s in summary:
            f.write(" * %s\n" % s)
        f.write(" */\n\n")
        f.write("/* Xlat11Class values are defined in xlat11.h. */\n\n")
        f.write("typedef uint32_t (*Xlat11Repack)(uint32_t);\n\n")
        f.write("typedef struct {\n"
                "    uint32_t     g10;     /* gfx10.3 MMIO byte address (sort key) */\n"
                "    uint32_t     g11;     /* gfx11 MMIO byte address; 0 if absent/unknown */\n"
                "    Xlat11Repack repack;  /* field_repack only, else 0 */\n"
                "    uint8_t      cls;     /* Xlat11Class */\n"
                "    const char  *name;\n"
                "    const char  *reason;  /* absent/legacy_vs/unknown: why */\n"
                "} Xlat11Entry;\n\n")
        f.write("#define XLAT11_G10_VGT_SHADER_STAGES_EN 0x%05xu /* %s */\n" % (ngg["addr"], ngg["cite"]))
        f.write("#define XLAT11_G10_PRIMGEN_EN_BIT       %du      /* NGG: gfx103.json VGT_SHADER_STAGES_EN */\n\n" % ngg["bit"])
        f.write("static const Xlat11Entry kXlat11Table[] = {\n")
        for a in sorted(entries):
            r = entries[a]
            rp = ("repack_%s" % r["name"]) if r["cls"] == "field_repack" else "0"
            f.write('    /* %s */\n' % cstr(r["cite"]))
            f.write('    { 0x%05xu, 0x%05xu, %s, XLAT11_CLS_%s, "%s", "%s" },\n'
                    % (a, r["g12"], rp, r["cls"].upper(), r["name"], cstr(r["reason"])))
        f.write("};\n#define XLAT11_TABLE_LEN ((uint32_t)(sizeof(kXlat11Table) / sizeof(kXlat11Table[0])))\n\n#endif\n")


if __name__ == "__main__":
    main()
