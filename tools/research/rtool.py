#!/usr/bin/env python3
"""rtool - a small command line front end for the venus RE toolkit.

Examples
--------
  # where is the string "ActivationState" in the binary?
  python3 tools/research/rtool.py findstr logs/mobileactivationd-20H392.bin ActivationState

  # what does the CFString pool around 0x215a08 contain?
  python3 tools/research/rtool.py pool logs/mobileactivationd-20H392.bin --addr 0x100215a08

  # which code uses the "ActivationState" key?
  python3 tools/research/rtool.py xrefs logs/mobileactivationd-20H392.bin --cfstring ActivationState

  # list the ObjC classes and their methods
  python3 tools/research/rtool.py objc logs/mobileactivationd-20H392.bin --class MobileActivationDaemon

  # disassemble a range
  python3 tools/research/rtool.py disasm logs/mobileactivationd-20H392.bin --start 0x100021270 --stop 0x100021290

  # dump a binary plist with the byte offset of every object
  python3 tools/research/rtool.py plist some.plist
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from research import bplist, macho  # noqa: E402


def _int(s):
    return int(s, 0)


def cmd_sections(args):
    m = macho.MachO(args.binary)
    print("text base: %#x" % m.text_base())
    for seg in m.segments:
        print(seg)
        for s in seg.sections:
            print("   ", s)
    if m.fixups:
        print(m.fixups)
    return 0


def cmd_findstr(args):
    m = macho.MachO(args.binary)
    hits = m.find_strings(args.text, args.section)
    if not hits:
        print("not found")
        return 1
    for addr, off in hits:
        print("vaddr %#012x  file offset %#08x  %r" % (addr, off, args.text))
    return 0


def cmd_pool(args):
    m = macho.MachO(args.binary)
    for e in m.pool_around(args.addr, args.count):
        off = m.vaddr_to_offset(e.slot)
        tgt = e.pointer.target if e.pointer else None
        print("slot %#012x (file %#08x) -> %-14s %r" % (
            e.slot, off if off is not None else -1,
            hex(tgt) if tgt else "?",
            e.text))
    return 0


def cmd_cfstrings(args):
    m = macho.MachO(args.binary)
    if args.text:
        for cs in m.cfstrings():
            if args.text in cs.text:
                print(cs)
        return 0
    for cs in m.cfstrings():
        print(cs)
    return 0


def cmd_objc(args):
    m = macho.MachO(args.binary)
    classes = m.objc_classes()
    if args.name:
        classes = [c for c in classes if c.name == args.name or args.name in c.name]
    for c in classes:
        print("%s  (super %s, %d instance methods, %d class methods)" % (
            c.name, c.superclass, len(c.methods), len(c.class_methods)))
        if args.methods:
            for meth in c.all_methods():
                print("    ", meth)
    return 0


def cmd_xrefs(args):
    m = macho.MachO(args.binary)
    targets = []
    if args.addr is not None:
        targets.append((args.addr, "address"))
    if args.text:
        for addr, off in m.find_strings(args.text):
            targets.append((addr, "string %r @file %#x" % (args.text, off)))
    if args.cfstring:
        for cs in m.cfstrings():
            if cs.text == args.cfstring:
                targets.append((cs.addr, "CFString %r" % cs.text))
                # the code normally points at the pointer array, not at the
                # CFString itself: look for the slots that reference it
                for slot in m.pool_slots_for(cs.addr):
                    targets.append((slot, "pool slot for %r" % cs.text))
    for target, what in targets:
        print("== xrefs of %#x (%s) ==" % (target, what))
        for x in m.xrefs(target):
            print("   ", x)
    return 0


def cmd_disasm(args):
    m = macho.MachO(args.binary)
    for line in m.disassemble(args.start, args.stop):
        raw = line.raw.hex(" ")
        print("%012x:\t%-11s\t%s" % (line.addr, raw, line.text))
    return 0


def cmd_plist(args):
    with open(args.file, "rb") as fh:
        data = fh.read()
    bp = bplist.Bplist(data)
    if args.key:
        obj = bp.find(args.key.split("/"))
        if obj is None:
            print("no such key")
            return 1
        print("%s  payload @%#x (%d bytes)" % (obj, obj.value_offset, obj.value_len))
        if args.patch:
            raw = bytes.fromhex(args.patch)
            data = bp.patch(args.key.split("/"), raw)
            bp = bplist.Bplist(data)
            print("patched ->", bp.find(args.key.split("/")))
            if args.out:
                with open(args.out, "wb") as fh:
                    fh.write(data)
                print("written to", args.out)
        return 0
    print(bp.tree(max_items=args.max_items))
    if args.patch:
        print("use --patch with --key")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="venus reverse engineering toolkit")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("sections", help="list the segments and sections")
    p.add_argument("binary")
    p.set_defaults(func=cmd_sections)

    p = sub.add_parser("findstr", help="find a string and print its file offset")
    p.add_argument("binary")
    p.add_argument("text")
    p.add_argument("--section", default="__cstring")
    p.set_defaults(func=cmd_findstr)

    p = sub.add_parser("pool", help="list the CFString pointer pool around an address")
    p.add_argument("binary")
    p.add_argument("--addr", type=_int, required=True)
    p.add_argument("--count", type=int, default=8)
    p.set_defaults(func=cmd_pool)

    p = sub.add_parser("cfstrings", help="list the CFString constants")
    p.add_argument("binary")
    p.add_argument("--text", default=None)
    p.set_defaults(func=cmd_cfstrings)

    p = sub.add_parser("objc", help="list the ObjC classes and their methods")
    p.add_argument("binary")
    p.add_argument("--class", dest="name", default=None)
    p.add_argument("--methods", action="store_true")
    p.set_defaults(func=cmd_objc)

    p = sub.add_parser("xrefs", help="find the references to an address/string")
    p.add_argument("binary")
    p.add_argument("--addr", type=_int, default=None)
    p.add_argument("--text", default=None)
    p.add_argument("--cfstring", default=None)
    p.set_defaults(func=cmd_xrefs)

    p = sub.add_parser("disasm", help="disassemble an address range")
    p.add_argument("binary")
    p.add_argument("--start", type=_int, default=None)
    p.add_argument("--stop", type=_int, default=None)
    p.set_defaults(func=cmd_disasm)

    p = sub.add_parser("plist", help="dump a binary plist with byte offsets")
    p.add_argument("file")
    p.add_argument("--key", default=None, help="key path, e.g. ActivationState")
    p.add_argument("--patch", default=None, help="hex bytes to write in place")
    p.add_argument("--out", default=None, help="write the patched plist here")
    p.add_argument("--max-items", type=int, default=None)
    p.set_defaults(func=cmd_plist)

    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
