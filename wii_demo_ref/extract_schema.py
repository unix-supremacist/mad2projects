#!/usr/bin/env python3
"""Extract per-class field name -> byte offset schema from Mad2.elf (Wii demo)
by decoding the trivial PPC load/store instructions inside each get*/set*
FromVariant/ToVariant accessor, rather than needing Ghidra's decompiler per-function.
"""
import re, struct, json, sys

ELF = "/home/unix/src/mad2assetextractor/wii_demo_ref/Mad2.elf"
SYMS = "/home/unix/src/mad2assetextractor/wii_demo_ref/Mad2.elf.symbols.txt"

TEXT_VADDR = 0x80006b20
TEXT_FILEOFF = 0x002d20
TEXT_SIZE = 0x4657d8

with open(ELF, "rb") as f:
    elf_data = f.read()

def read_text(addr, size):
    off = TEXT_FILEOFF + (addr - TEXT_VADDR)
    if off < 0 or off + size > len(elf_data):
        return b""
    return elf_data[off:off+size]

# Parse Metrowerks-style qualified name: Q<N><len><name><len><name>...
def parse_qualified(s):
    m = re.match(r'^Q(\d)(.*)$', s)
    if not m:
        return [s], s
    n = int(m.group(1))
    rest = m.group(2)
    parts = []
    for _ in range(n):
        m2 = re.match(r'^(\d+)', rest)
        if not m2:
            break
        ln = int(m2.group(1))
        rest = rest[len(m2.group(1)):]
        parts.append(rest[:ln])
        rest = rest[ln:]
    return parts, rest

sym_re = re.compile(r'^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+\S+\s+(\S+)\s*$')
# get<Field>ToVariant / set<Field>FromVariant, possibly const (C) before F
acc_re = re.compile(r'^(get|set)([A-Za-z0-9_]+?)(ToVariant|FromVariant)__(Q\d+\S+)$')

results = {}  # class_path (str) -> {field: {offsets:[...], kind:...}}
count_matched = 0
count_decoded = 0

with open(SYMS) as f:
    for line in f:
        m = sym_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        size = int(m.group(2))
        name = m.group(3)
        if size == 0 or size > 128:
            continue
        am = acc_re.match(name)
        if not am:
            continue
        verb, field, suffix, qual = am.groups()
        count_matched += 1
        field = field[0].lower() + field[1:]

        parts, rest = parse_qualified(qual)
        class_path = "::".join(parts)

        code = read_text(addr, size)
        if not code:
            continue
        offsets_int = []
        offsets_float = []
        n_instr = len(code) // 4
        for i in range(n_instr):
            word = struct.unpack_from(">I", code, i*4)[0]
            opcode = (word >> 26) & 0x3F
            rA = (word >> 16) & 0x1F
            d = word & 0xFFFF
            if d >= 0x8000:
                d -= 0x10000
            if rA != 3:
                continue
            # Offset 0 is always the classIdx/vtable slot, never a real
            # field — a match there means we decoded the wrong instruction
            # (e.g. a stub/thunk that doesn't actually touch `this`), not a
            # real field access. Confirmed by cross-checking against real
            # level.bld object graph reads: every offset==0 entry produced
            # nonsense (all-identical values across distinct instances).
            if d == 0:
                continue
            if opcode in (32, 36):  # lwz, stw
                offsets_int.append(d)
            elif opcode in (48, 52):  # lfs, stfs
                offsets_float.append(d)
            elif opcode in (50, 54):  # lfd, stfd (double, rare)
                offsets_float.append(d)

        all_offsets = sorted(set(offsets_int) | set(offsets_float))
        if not all_offsets:
            continue
        count_decoded += 1

        kind = "float" if offsets_float and not offsets_int else ("int" if offsets_int and not offsets_float else "mixed")

        cls = results.setdefault(class_path, {})
        fld = cls.setdefault(field, {"offsets": set(), "kind": set(), "accessors": []})
        fld["offsets"].update(all_offsets)
        fld["kind"].add(kind)
        fld["accessors"].append(f"{verb}:{name}@0x{addr:x}")

# finalize
out = {}
for cls, fields in results.items():
    out[cls] = {}
    for field, info in fields.items():
        offs = sorted(info["offsets"])
        out[cls][field] = {
            "offset": offs[0] if len(offs) == 1 else offs,
            "kind": sorted(info["kind"]),
            "accessors": info["accessors"],
        }

print(f"Matched {count_matched} accessor-shaped symbols; decoded offsets for {count_decoded}", file=sys.stderr)
print(f"Classes with at least one resolved field: {len(out)}", file=sys.stderr)

with open(sys.argv[1] if len(sys.argv) > 1 else "schema.json", "w") as f:
    json.dump(out, f, indent=2, sort_keys=True)
