#!/usr/bin/env python3
"""Self test for the venus RE toolkit.

Run it with:

    python3 tools/research/selftest.py

It checks the three modules against synthetic data and against the real
binary in logs/mobileactivationd-20H392.bin (the tests that need it are
skipped when the file is not there).
"""

from __future__ import annotations

import os
import plistlib
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from research import asm, bplist, macho  # noqa: E402

BINARY = os.path.join(ROOT, "logs", "mobileactivationd-20H392.bin")

_failures = []
_checks = 0


def check(cond, what):
    global _checks
    _checks += 1
    if cond:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        _failures.append(what)


def check_eq(got, want, what):
    check(got == want, "%s (got %r, want %r)" % (what, got, want))


# ---------------------------------------------------------------------------
# bplist
# ---------------------------------------------------------------------------


def test_bplist():
    print("bplist:")
    src = {
        "ActivationState": "Activated",
        "ActivationRecord": b"\x01\x02\x03\x04",
        "count": 42,
        "big": 2 ** 40,
        "neg": -7,
        "real": 1.5,
        "yes": True,
        "no": False,
        "list": [1, 2, 3],
        "long": "x" * 300,
    }
    blob = plistlib.dumps(src, fmt=plistlib.FMT_BINARY)
    bp = bplist.Bplist(blob)

    check_eq(bplist.to_python(bp.root), src, "plistlib round trip")
    check(bp.num_objects == len(bp.offsets), "the object table is complete")

    # every object offset must point at its own marker byte
    ok = True
    for i, off in enumerate(bp.offsets):
        obj = bp.object(i)
        if obj.offset != off:
            ok = False
    check(ok, "every object knows its own byte offset")

    # the offsets have to be usable to patch a value in place
    obj = bp.find(["ActivationState"])
    check(obj is not None and obj.value() == "Activated", "find() the key")
    check_eq(blob[obj.value_offset:obj.value_offset + obj.value_len], b"Activated",
             "the payload is where the object says it is")
    patched = bp.set_string(["ActivationState"], "Xctivated")
    check_eq(len(patched), len(blob), "patching does not change the file size")
    check_eq(bplist.Bplist(patched).find(["ActivationState"]).value(), "Xctivated",
             "the patched value is read back")
    check_eq(blob[obj.value_offset:obj.value_offset + obj.value_len], b"Activated",
             "the original buffer was not modified")

    # the key offsets helper
    keys = dict((k, (ko, vo)) for k, ko, vo in bp.key_offsets())
    check("ActivationState" in keys, "key_offsets() lists the keys")
    check_eq(keys["ActivationState"][1], obj.offset, "key_offsets() agrees with find()")

    # 16 byte integers and negative numbers survive
    check_eq(bplist.to_python(bp.root)["big"], 2 ** 40, "a big int")
    check_eq(bplist.to_python(bp.root)["neg"], -7, "a negative int")

    # the 0x4F variant: "data with the count in the next object"
    inline = _make_bplist_0x4f_inline()
    bp2 = bplist.Bplist(inline)
    check_eq(bplist.to_python(bp2.root)["blob"], b"\x01\x02\x03\x04\x05",
             "0x4F data with an inline int count")
    table = _make_bplist_0x4f_table()
    bp3 = bplist.Bplist(table)
    check_eq(bplist.to_python(bp3.root)["blob"], b"\x01\x02\x03\x04\x05",
             "0x4F data with the count in the next table object")

    # UTF-16 strings and 16 byte integers
    utf = _make_bplist_utf16()
    bp4 = bplist.Bplist(utf)
    check_eq(bp4.find(["utf"]).value(), "Aktivierung", "a UTF-16BE string")
    check_eq(bp4.find(["utf"]).encoding, "utf-16-be", "the encoding is remembered")
    check_eq(bp4.object(3).value(), 1, "a 16 byte integer")


def _make_bplist_utf16():
    """A hand made plist with a UTF-16BE string and a 16 byte integer."""
    text = "Aktivierung".encode("utf-16-be")
    parts, offsets = [], []
    pos = 8

    def add(blob):
        nonlocal pos
        offsets.append(pos)
        pos += len(blob)
        parts.append(blob)

    add(b"\xd1\x01\x02")                               # object 0: dict
    add(b"\x53utf")                                    # object 1: "utf"
    add(bytes([0x60 | (len(text) // 2)]) + text)       # object 2: UTF-16 string
    add(b"\x14" + (1).to_bytes(16, "big", signed=True))  # object 3: 16 byte int
    body = b"".join(parts)
    table = b"".join(struct.pack(">B", o) for o in offsets)
    trailer = struct.pack(">5xBBBQQQ", 0, 1, 1, len(offsets), 0, 8 + len(body))
    return b"bplist00" + body + table + trailer


def _make_bplist_0x4f_inline():
    """Hand made bplist with a 0x4F data object (count as an inline int)."""
    parts, offsets = [], []
    pos = 8

    def add(blob):
        nonlocal pos
        offsets.append(pos)
        pos += len(blob)
        parts.append(blob)

    add(b"\xd1\x01\x02")                          # object 0: dict, key=1 value=2
    add(b"\x54blob")                               # object 1: "blob"
    add(b"\x4f\x10\x05" + b"\x01\x02\x03\x04\x05")  # object 2: data, count 5
    body = b"".join(parts)
    table = b"".join(struct.pack(">B", o) for o in offsets)
    trailer = struct.pack(">5xBBBQQQ", 0, 1, 1, len(offsets), 0, 8 + len(body))
    return b"bplist00" + body + table + trailer


def _make_bplist_0x4f_table():
    """Hand made bplist with a 0x4F data object whose count is the next
    object of the table (the variant the CFBinaryPlist documentation
    describes)."""
    parts, offsets = [], []
    pos = 8

    def add(blob):
        nonlocal pos
        offsets.append(pos)
        pos += len(blob)
        parts.append(blob)

    add(b"\xd1\x01\x02")                      # object 0: dict, key=1 value=2
    add(b"\x54blob")                           # object 1: "blob"
    add(b"\x4f" + b"\x01\x02\x03\x04\x05")     # object 2: data (count in obj 3)
    add(b"\x10\x05")                           # object 3: the int 5
    body = b"".join(parts)
    table = b"".join(struct.pack(">B", o) for o in offsets)
    trailer = struct.pack(">5xBBBQQQ", 0, 1, 1, len(offsets), 0, 8 + len(body))
    return b"bplist00" + body + table + trailer


# ---------------------------------------------------------------------------
# asm
# ---------------------------------------------------------------------------


def test_asm():
    print("asm:")
    # ldr x3, [x8, #0xa08]  (the "site A" of mobileactivationd)
    w = 0xF9450503
    i = asm.decode(w, pc=0x10002127C)
    check_eq(i.mnemonic, "ldr", "decode ldr")
    check_eq(i.fields["rt"], 3, "ldr rt")
    check_eq(i.fields["rn"], 8, "ldr rn")
    check_eq(i.fields["imm"], 0xA08, "ldr imm (scaled)")
    check_eq(asm.encode("ldr", rt=3, rn=8, imm=0xA10), 0xF9450903,
             "encode the patched ldr")
    check_eq(asm.encode("ldr", rt=3, rn=8, imm=0xA08), w, "ldr encode round trip")

    # cbnz x19, ...  (site B) and tbz/tbnz
    i = asm.decode(0xB50000D3, pc=0x100005A18)
    check_eq(i.mnemonic, "cbnz", "decode cbnz")
    check_eq(i.fields["rt"], 19, "cbnz rt")
    check_eq(i.fields["target"], 0x100005A30, "cbnz target")
    i = asm.decode(0x36000D60, pc=0x100005A44)
    check(i.mnemonic in ("tbz", "tbnz"), "decode tbz/tbnz")
    check_eq(i.fields["rt"] & 0x1F, 0, "tbz register")

    # adrp
    i = asm.decode(0x90001168, pc=0x100004780)
    check_eq(i.mnemonic, "adrp", "decode adrp")
    check_eq(i.fields["page"], 0x100230000, "adrp page")
    check_eq(asm.encode("adrp", rd=8, page=0x100230000, pc=0x100004780), 0x90001168,
             "adrp encode round trip")

    # movz / movk
    check_eq(asm.encode("movz", rd=14, imm16=2, sf=0), 0x5280004E, "movz encode")
    check_eq(asm.encode("movz", rd=0, imm16=1), 0xD2800020, "movz x0 encode")
    check_eq(asm.encode("movk", rd=9, imm16=0x840, shift=16, sf=0), 0x72A10809,
             "movk encode")
    check_eq(asm.decode(0x72A10809).fields["imm16"], 0x840, "movk imm16")
    check_eq(asm.decode(0x72A10809).fields["shift"], 16, "movk shift")

    # branches
    w = asm.encode("bl", target=0x1001BFE50, pc=0x1000047B0)
    check_eq(w, 0x9406EDA8, "bl encode")
    check_eq(asm.branch_target(w, 0x1000047B0), 0x1001BFE50, "bl target")
    w = asm.encode("b.cond", cond="ne", target=0x1000047A0, pc=0x10000478C)
    check_eq(w, 0x540000A1, "b.cond encode")
    check_eq(asm.decode(w, pc=0x10000478C).fields["target"], 0x1000047A0,
             "b.cond target")
    w = asm.encode("cbz", rt=0, target=0x100004860, pc=0x100004814)
    check_eq(w, 0xB4000260, "cbz encode")
    w = asm.encode("tbnz", rt=8, bit=31, target=0x100004C10, pc=0x100004AE8)
    check_eq(w, 0x37F80948, "tbnz encode")

    # nop and the retarget helper
    check_eq(asm.encode("nop"), 0xD503201F, "nop encode")
    w = asm.encode("b", target=0x100004790, pc=0x1000047B4)
    check_eq(w, 0x17FFFFF7, "b encode")
    check_eq(asm.retarget(w, 0x1000047B4, 0x100004800), 0x14000013,
             "retarget a branch")

    # add (immediate) - needed for the adrp+add xref scan
    check_eq(asm.decode(0x9110C000, pc=0x1000047A4).fields["imm12"], 0x430,
             "add imm12")


# ---------------------------------------------------------------------------
# macho
# ---------------------------------------------------------------------------


def test_macho():
    print("macho:")
    if not os.path.exists(BINARY):
        print("  SKIP %s is missing" % BINARY)
        return
    m = macho.MachO(BINARY)

    check_eq(m.text_base(), 0x100000000, "text base")
    check_eq(m.vaddr_to_offset(0x10002127C), 0x2127C, "vaddr -> file offset")
    check_eq(m.offset_to_vaddr(0x2127C), 0x10002127C, "file offset -> vaddr")
    check(m.section("__cfstring") is not None, "the __cfstring section is there")
    check_eq(m.section("__cfstring").addr, 0x100220458, "__cfstring address")

    # the CFString pool around 0x215a08
    pool = m.cfstring_pool(0x100215A08, 8)
    texts = [e.text for e in pool]
    check("Unactivated" in texts, "the pool has 'Unactivated'")
    check("Activated" in texts, "the pool has 'Activated'")
    check("FactoryActivated" in texts, "the pool has 'FactoryActivated'")
    un = [e for e in pool if e.text == "Unactivated"][0]
    check_eq(un.slot, 0x100215A08, "the 'Unactivated' slot address")
    check_eq(un.cfstring.addr, 0x100225F58, "the CFString object address")
    check_eq(un.cfstring.length, 11, "the CFString length")
    check_eq(un.cfstring.cstring, 0x100208865, "the CFString text address")

    # the keys
    for text, want_off in (("ActivationState", 0x208BFC), ("ActivationRecord", 0x208BD4)):
        hits = m.find_strings(text)
        check(any(off == want_off for _, off in hits),
              "the file offset of %r is %#x" % (text, want_off))
    slots = m.pool_slots_for(0x100226458)     # the CFString of "ActivationState"
    check(0x100215B60 in slots, "the pool slot of 'ActivationState' is 0x100215b60")
    slots = m.pool_slots_for(0x1002263D8)     # the CFString of "ActivationRecord"
    check(0x100215B38 in slots, "the pool slot of 'ActivationRecord' is 0x100215b38")

    # ObjC
    classes = m.objc_classes()
    check(len(classes) >= 9, "the ObjC class list is parsed (%d classes)" % len(classes))
    mad = m.objc_class_named("MobileActivationDaemon")
    check(mad is not None, "MobileActivationDaemon is found")
    if mad is not None:
        check_eq(len(mad.methods), 57, "MobileActivationDaemon has 57 methods")
        names = [x.name for x in mad.methods]
        check("initWithContext:Queue:" in names, "the method names are resolved")
        check(all(n for n in names), "every method has a name")
        imp = [x for x in mad.methods if x.name == "initWithContext:Queue:"][0]
        check_eq(imp.imp, 0x100040768, "the IMP of initWithContext:Queue:")

    # xrefs
    xrefs = m.xrefs(0x100215A08)
    addrs = [x.addr for x in xrefs]
    check(0x10002127C in addrs, "site A (0x10002127c) references the state pool")
    check(0x100024DEC in addrs, "the reply path references the state pool")
    xrefs = m.xrefs(0x100215B60)
    addrs = [x.addr for x in xrefs]
    check(0x100021270 in addrs, "the 'ActivationState' key is loaded at 0x100021270")

    # the disassembly
    lines = m.disassemble(0x100021270, 0x100021280)
    check_eq(len(lines), 4, "the disassembly of a small range")
    check_eq(lines[1].text.split("\t")[0], "adrp", "the second instruction is an adrp")

    # the chained fixups
    check(m.fixups is not None, "the chained fixups are parsed")
    if m.fixups:
        check_eq(m.fixups.segments[0].pointer_format, 6,
                 "the pointer format is DYLD_CHAINED_PTR_64_OFFSET")
        check(len(m.fixups.imports) > 100, "the import table is parsed")

    # the decoder must agree with llvm-objdump on the real code
    checked = 0
    bad = 0
    for line in m.disassemble()[:40000]:
        try:
            ins = asm.decode(line.word, pc=line.addr)
        except ValueError:
            continue
        if ins.mnemonic in ("b", "bl", "b.cond", "cbz", "cbnz", "tbz", "tbnz"):
            mm = re.search(r"(0x[0-9a-f]+)\s*(?:;.*)?$", line.text)
            if mm:
                checked += 1
                if int(mm.group(1), 16) != ins.fields["target"]:
                    bad += 1
        elif ins.mnemonic in ("ldr", "str"):
            mm = re.search(r"#(0x[0-9a-f]+)", line.text)
            if mm:
                checked += 1
                if int(mm.group(1), 16) != ins.fields["imm"]:
                    bad += 1
        elif ins.mnemonic == "adrp":
            mm = re.search(r"(0x[0-9a-f]+)\s*;", line.text)
            if mm:
                checked += 1
                if int(mm.group(1), 16) != ins.fields["page"]:
                    bad += 1
    check(bad == 0, "the decoder agrees with llvm-objdump (%d instructions)" % checked)


# ---------------------------------------------------------------------------


def main():
    print("venus RE toolkit self test")
    print("==========================")
    test_bplist()
    test_asm()
    test_macho()
    print("")
    if _failures:
        print("FAILED %d of %d checks" % (len(_failures), _checks))
        for f in _failures:
            print("  - %s" % f)
        return 1
    print("all %d checks passed" % _checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
