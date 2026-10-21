# venus RE toolkit

Small, dependency free (python3 stdlib only) helpers for looking at iOS
binaries on the Mac, so that the next agent does not have to write another
one-off parser.

```
tools/research/
  bplist.py     binary property list reader with byte offsets (patch in place)
  macho.py      arm64 Mach-O: sections, chained fixups, CFString pool,
                ObjC classes/methods, disassembly, adrp+add xref search
  asm.py        arm64 encode/decode for the instructions we patch
  rtool.py      command line front end for all of it
  selftest.py   the self test (must pass)
```

Everything works on files on disk; nothing here talks to the device.

## Self test

```sh
python3 tools/research/selftest.py
```

It checks the three modules against synthetic data *and* against the real
binary in `logs/mobileactivationd-20H392.bin` (the checks that need that file
are skipped when it is not there).  Current state: **74 checks, all pass**
(~4 s, most of it the llvm-objdump run).

## Command line

```sh
B=logs/mobileactivationd-20H392.bin

# where is the string "ActivationState" in the file?
python3 tools/research/rtool.py findstr $B ActivationState
#   vaddr 0x0100208bfc  file offset 0x208bfc  'ActivationState'

# what is in the CFString pointer pool around 0x215a08?
python3 tools/research/rtool.py pool $B --addr 0x100215a08
#   slot 0x0100215a08 (file 0x215a08) -> 0x100225f58    'Unactivated'
#   slot 0x0100215a10 (file 0x215a10) -> 0x100225f78    'Activated'
#   slot 0x0100215a18 (file 0x215a18) -> 0x100225f98    'FactoryActivated'

# which code uses the "ActivationState" key?
python3 tools/research/rtool.py xrefs $B --cfstring ActivationState

# the ObjC classes and their methods
python3 tools/research/rtool.py objc $B --class MobileActivationDaemon --methods

# a range of code
python3 tools/research/rtool.py disasm $B --start 0x100021270 --stop 0x100021290
#   000100021270: 02 b1 45 f9  ldr  x2, [x8, #0xb60]
#   000100021274: a8 0f 00 90  adrp x8, 500 ; 0x100215000
#   000100021278: 1f 20 03 d5  nop
#   00010002127c: 03 05 45 f9  ldr  x3, [x8, #0xa08]

# the segments, sections and the chained fixups
python3 tools/research/rtool.py sections $B

# a binary plist
python3 tools/research/rtool.py plist some.plist
python3 tools/research/rtool.py plist some.plist --key ActivationState \
        --patch 416374697661746564 --out patched.plist
```

## bplist.py

`plistlib` cannot read binary plists at all, and when you *can* read one you
usually want to change one value in it.  `bplist.py` reads `bplist00` and
remembers the byte offset of every object, so a value can be patched in place.

```python
import sys; sys.path.insert(0, "tools")
from research import bplist

bp = bplist.Bplist(open("purplebuddy.plist", "rb").read())
print(bp.tree())            # the tree with the offset of every object
obj = bp.find(["ActivationState"])
print(hex(obj.offset), hex(obj.value_offset), obj.value_len)

new = bp.set_string(["ActivationState"], "Activated")   # same length!
open("out.plist", "wb").write(new)
```

* objects: dict, array, set, ASCII string, UTF-16BE string, int (1/2/4/8/16
  bytes, signed), real (4/8 bytes), data, bool, date, UID, null
* the "0x?F" extended size is handled in both flavours: the count as an
  inline integer right behind the marker (what Apple/plutil and plistlib
  write) and the count in the next object of the table (what the
  CFBinaryPlist documentation describes)
* `Bplist.patch(path, raw_bytes)` writes bytes of the *same length* into the
  payload of the object at `path`; `set_string()` does the same for a string
* `Bplist.key_offsets()` lists every key with the offset of the key object and
  of the value object

## macho.py

```python
import sys; sys.path.insert(0, "tools")
from research import macho

m = macho.MachO("logs/mobileactivationd-20H392.bin")
m.sections                 # every section with addr/size/file offset
m.vaddr_to_offset(0x10002127c)   # -> 0x2127c
m.offset_to_vaddr(0x2127c)       # -> 0x10002127c
m.find_strings("ActivationState")   # -> [(vaddr, file offset), ...]
m.cfstrings()               # every CFString constant of __cfstring
m.pool_around(0x100215a08)  # the pointer array around an address
m.objc_classes()            # the ObjC classes with their methods and IMPs
m.disassemble(0x100021270, 0x100021290)
m.xrefs(0x100215a08)        # the adrp+add / adrp+ldr references
```

Addresses are the *unslid* ones (0x10002127c), i.e. the ones you see in a hex
editor; for this binary the file offset is simply `vaddr - 0x100000000`.

### The CFString pool

A `__CFString` object is 32 bytes: `isa`, `flags` (the 32 bit `_cfinfo`),
`cString` (a pointer) and `length`.  The code of a binary does **not**
reference those objects directly: it references an array of *pointers* to
them, and that array lives in `__DATA_CONST,__const`.  So:

```python
for e in m.pool_around(0x100215a08, 8):
    print(hex(e.slot), e.text)
# 0x100215a08 Unactivated
# 0x100215a10 Activated
# 0x100215a18 FactoryActivated
```

`m.pool_slots_for(cfstring_addr)` goes the other way: it finds the pool slots
that point at a given CFString (that is how you find the "ActivationState"
key: the CFString at 0x100226458 is referenced by the slot at 0x100215b60).

### Chained fixups

`LC_DYLD_CHAINED_FIXUPS` is parsed (header, the per segment chain starts and
the import table), so the pointers in `__DATA`/`__DATA_CONST` can be turned
back into addresses.  mobileactivationd uses
`DYLD_CHAINED_PTR_64_OFFSET` (format 6): a 36 bit offset from the image base
plus a 12 bit "next" field, and the top bit of the word marks a *bind* (an
imported symbol, e.g. the `isa` of a CFString, which is
`_OBJC_CLASS_$_CFString` in CoreFoundation).

`m.pointer(addr)` returns a `Pointer` with `kind` (`rebase`, `bind`,
`plain`), the resolved `target` and - for a bind - the import `name`.

### ObjC

The class list, the class_ro_t and the method lists are parsed, including
the **relative method lists** (`entsize & 0x80000000`) that modern Apple
binaries use.  In those, every field of a method entry is a signed 32 bit
offset **relative to the address of the field itself**:

```
entry + 0   : int32 name   -> the selector reference (in __objc_selrefs)
entry + 4   : int32 types  -> the type string   (in __objc_methtype)
entry + 8   : int32 imp    -> the implementation
```

The selector reference is a pointer to the real name in `__objc_methname`,
so the name is two indirections away.  `otool -oV` cannot resolve that (it
prints the raw offset), this module does.

`MobileActivationDaemon` has 57 methods; `initWithContext:Queue:` is at
IMP 0x100040768, which is what `otool -oV` says as well.

### Disassembly and xrefs

The disassembly uses `llvm-objdump --macho --disassemble` (found via
`xcrun -f llvm-objdump`, the Xcode toolchain path, or `$VENUS_LLVM_OBJDUMP`;
`otool -tvV` is the fallback).  Note that llvm-objdump ignores
`--start-address`/`--stop-address` for Mach-O, so the whole `__text` is
disassembled once (0.2 s) and cached, then filtered.

The xref search decodes the instructions with our own `asm.py` and tracks
the `adrp` pages per register, so both patterns are found:

```
adrp x8, 0x100215000 ; add x8, x8, #0xa08   -> the address of the pointer
adrp x8, 0x100215000 ; ldr x3, [x8, #0xa08]  -> the pointer itself
```

## asm.py

Encode/decode of the instructions we actually patch.  Every encoding was
checked against `llvm-objdump` (the self test compares 15 000+ decoded
instructions with the disassembler output).

```python
from research import asm

i = asm.decode(0xF9450503, pc=0x10002127C)
i.mnemonic          # 'ldr'
i.fields            # {'size': 3, 'rt': 3, 'rn': 8, 'imm': 0xa08, ...}

asm.encode("ldr", rt=3, rn=8, imm=0xA10)     # 0xf9450903  (the patch!)
asm.decode(0x90001168, pc=0x100004780).fields['page']   # 0x100230000
asm.encode("adrp", rd=8, page=0x100230000, pc=0x100004780)
asm.encode("bl", target=0x1001BFE50, pc=0x1000047B0)
asm.branch_target(0x9406EDA8, 0x1000047B0)    # 0x1001bfe50
asm.retarget(word, pc, new_target)           # move a branch
```

Supported: `ldr`/`str` (immediate, unsigned offset, 12 bit scaled imm),
`movz`/`movn`/`movk`, `adr`/`adrp`, `b`/`bl`, `b.cond`, `cbz`/`cbnz`,
`tbz`/`tbnz`, `add`/`sub` (immediate, needed for the xref scan) and `nop`.

## Things to remember

* the file offset of a vaddr is `vaddr - 0x100000000` for this binary, but
  use `vaddr_to_offset()` - it is not always true
* the CFString *text* is in `__TEXT,__cstring`, the CFString *objects* in
  `__DATA_CONST,__cfstring` and the pointers to them in
  `__DATA_CONST,__const`; three different places
* `otool -oV` prints the raw relative offsets of the method names; the
  section relative base mentioned in older notes does not work for this
  binary, the field relative one does
* patching a string in a binary plist only works if the new text has the
  same length
