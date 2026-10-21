# 13 - the venus RE toolkit (tools/research/)

**Date:** 2026-10-21
**What:** a reusable python3 toolkit for analysing iOS binaries, so that the
next agent does not have to write another one-off parser.

## Why

Everything we know about `mobileactivationd` so far was found by hand, one
script at a time: a plist parser, a chained fixup decoder, a CFString
resolver, an ObjC reader, an xref scanner.  All of it was thrown away after
the answer was found, and some of it was wrong (see "what we got wrong"
below).  This toolkit puts the reusable parts in one place, with a self
test that proves they agree with `llvm-objdump` and with `plistlib`.

## What is in it

```
tools/research/
  README.md     the documentation (usage examples for everything)
  __init__.py
  bplist.py     binary property list reader with byte offsets
  macho.py      arm64 Mach-O reader
  asm.py        arm64 instruction encode/decode
  rtool.py      command line front end
  selftest.py   the self test:  python3 tools/research/selftest.py
```

stdlib only, no pip installs, macOS only for the disassembly (it needs
`llvm-objdump`, which ships with the Xcode command line tools; `otool` is the
fallback).

## bplist.py - binary plists with byte offsets

`plistlib` cannot *read* binary plists at all, and the plists we care about
(`com.apple.purplebuddy.plist`, the activation data ark) are binary.  The
module reads `bplist00` and remembers the byte offset of every object, so a
value can be patched in place.

* objects: dict, array, set, ASCII string, UTF-16BE string, int (1/2/4/8/16
  bytes, signed), real (4/8 bytes), data, bool, date, UID, null
* the extended size (`0x?F`) is handled in both flavours: the count as an
  inline integer right behind the marker (what Apple/plutil and plistlib
  write) and the count in the next object of the object table (what the
  CFBinaryPlist documentation describes)
* `Bplist.tree()` prints the tree with the offset of every object,
  `Bplist.find(path)` looks a key up, `Bplist.patch()` / `set_string()`
  write into the payload of an object (same length only)
* verified against `plistlib` and against a file `plutil` produced

## macho.py - the Mach-O side

* segments, sections, `vaddr_to_offset()` / `offset_to_vaddr()`
* `LC_DYLD_CHAINED_FIXUPS`: header, the per segment chain starts and the
  import table, so the pointers in `__DATA`/`__DATA_CONST` become addresses
  again.  mobileactivationd uses `DYLD_CHAINED_PTR_64_OFFSET` (format 6):
  a 36 bit offset from the image base plus a 12 bit "next" field; the top
  bit of the word marks a *bind* (an imported symbol - that is what the
  `isa` of a CFString is: `_OBJC_CLASS_$_CFString`).
* the CFString pool: the `__CFString` objects (isa, flags, cString pointer,
  length) and the arrays of pointers to them
* the ObjC classes with their methods and IMPs, including the **relative
  method lists**
* the disassembly (`llvm-objdump --macho --disassemble`) and the
  adrp+add / adrp+ldr xref search

### The CFString pool of mobileactivationd (20H392)

The three places that matter:

| what | where |
|---|---|
| the text | `__TEXT,__cstring` (file offset = vaddr - 0x100000000) |
| the `__CFString` objects | `__DATA_CONST,__cfstring` (0x100220458) |
| the pointers to them | `__DATA_CONST,__const` (0x100214a80) |

```
slot 0x100215a08 (file 0x215a08) -> 0x100225f58  'Unactivated'
slot 0x100215a10 (file 0x215a10) -> 0x100225f78  'Activated'
slot 0x100215a18 (file 0x215a18) -> 0x100225f98  'FactoryActivated'
slot 0x100215b38 (file 0x215b38) -> 0x1002263d8  'ActivationRecord'
slot 0x100215b60 (file 0x215b60) -> 0x100226458  'ActivationState'
```

The key strings themselves:

```
'ActivationState'  vaddr 0x100208bfc  file offset 0x208bfc
'ActivationRecord' vaddr 0x100208bd4  file offset 0x208bd4
```

### The relative method lists (this is the important bit)

Modern Apple binaries use method lists with `entsize & 0x80000000`.  In
those, **every field of a method entry is a signed 32 bit offset relative to
the address of the field itself**:

```
entry + 0   int32 name   -> the selector reference (in __objc_selrefs)
entry + 4   int32 types  -> the type string       (in __objc_methtype)
entry + 8   int32 imp    -> the implementation
```

The selector reference is a pointer to the real name in `__objc_methname`,
so the name is two indirections away.  `otool -oV` cannot resolve that (it
prints `name 0x6b1e8 (0x10022e788)`), this module can.

**What we got wrong earlier:** the older notes say the name offset is
relative to the `__objc_methname` section base.  That is *not* what this
binary does - `__objc_methname` is at 0x10020c43e and the offsets are
0x6b1e8 and bigger, so that interpretation points outside the file.  The
field relative interpretation is the one that works, and it reproduces
`otool`'s addresses exactly.

With that, `MobileActivationDaemon` (0x10022f3a0) has 57 methods; the first
one is `-[MobileActivationDaemon initWithContext:Queue:]` with IMP
0x100040768 - the same IMP `otool` prints.

### The xrefs

The xref scanner decodes the instructions with our own `asm.py`, tracks the
`adrp` page per register and reports both patterns:

```
adrp x8, 0x100215000 ; add x8, x8, #0xa08   -> the address of the pointer
adrp x8, 0x100215000 ; ldr x3, [x8, #0xa08]  -> the pointer itself
```

For the state pool (0x100215a08) it finds 23 references, among them the two
sites of the activation patch:

```
0x100005a24  ldr x0, [x8, #0xa08]   (site B: the nil fallback)
0x10002127c  ldr x3, [x8, #0xa08]   (site A: the data ark default)
0x100024dec  ldr x0, [x8, #0xa08]   (the reply path)
```

and for the `ActivationState` key (pool slot 0x100215b60) 47 references,
among them 0x100021270 - the key load right in front of site A.

## asm.py - the instructions we patch

`ldr`/`str` (immediate, unsigned offset, 12 bit scaled imm), `movz`/`movn`/
`movk`, `adr`/`adrp`, `b`/`bl`, `b.cond`, `cbz`/`cbnz`, `tbz`/`tbnz`,
`add`/`sub` (immediate) and `nop`.

The encodings were not taken from a book but measured: the self test
decodes 15 872 instructions of the real binary and compares the immediates
and the branch targets with the `llvm-objdump` output (0 mismatches).

The patch of the activation work, computed with it:

```python
asm.encode("ldr", rt=3, rn=8, imm=0xA10)   # 0xf9450903  (was 0xf9450503)
```

## How to use it

```sh
python3 tools/research/selftest.py                    # must pass

B=logs/mobileactivationd-20H392.bin
python3 tools/research/rtool.py findstr $B ActivationState
python3 tools/research/rtool.py pool    $B --addr 0x100215a08
python3 tools/research/rtool.py xrefs   $B --cfstring ActivationState
python3 tools/research/rtool.py objc    $B --class MobileActivationDaemon --methods
python3 tools/research/rtool.py disasm  $B --start 0x100021270 --stop 0x100021290
python3 tools/research/rtool.py plist   some.plist
```

or from python:

```python
import sys; sys.path.insert(0, "tools")
from research import bplist, macho, asm

m = macho.MachO("logs/mobileactivationd-20H392.bin")
print(m.pool_around(0x100215a08, 8))
print(m.xrefs(0x100215a08))
print(m.objc_class_named("MobileActivationDaemon").methods[:3])
```

## Limits

* the xref scanner only understands the instructions of `asm.py`; an
  unknown instruction in between may leave a stale register value and cause
  a false positive (every hit prints the instruction, so it is easy to
  check)
* the ObjC reader does not do categories and does not resolve the
  superclass (it lives in the dyld shared cache and is a bind)
* `llvm-objdump` ignores `--start-address` for Mach-O, so the whole `__text`
  is disassembled once per process (0.2 s) and cached
* the bplist reader does not write plists, it only patches them in place
  (same length)
