"""arm64 Mach-O helpers for iOS binaries.

What it does:

  * parse the load commands, segments and sections of a 64 bit Mach-O
  * map virtual addresses <-> file offsets
  * decode the chained fixups (LC_DYLD_CHAINED_FIXUPS) so that pointers in
    __DATA / __DATA_CONST can be turned back into addresses
  * list the CFString constant pool (__cfstring) and the arrays of CFString
    pointers that live in __DATA_CONST,__const, and resolve them to their text
  * list the ObjC classes and their methods (including the relative method
    lists used by modern Apple binaries) with their IMPs
  * disassemble an address range with llvm-objdump and find the adrp+add
    xrefs of an address

Everything is stdlib only; llvm-objdump is used for the disassembly (it ships
with the Xcode command line tools, see ``find_llvm_objdump``).

The address conventions used here are the ones of an *unslid* image: the
addresses are the ones you see in a hex editor (``0x10002127c`` for the
``ldr x3, [x8, #0xa08]`` of mobileactivationd) and are therefore also the
addresses dyld would relocate to, before the ASLR slide is added.
"""

from __future__ import annotations

import glob
import os
import re
import shutil
import struct
import subprocess
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

try:
    from . import asm
except ImportError:      # pragma: no cover - "import macho" without the package
    import asm

__all__ = [
    "MachO", "Section", "Segment", "CFString", "ObjCClass", "ObjCMethod",
    "Xref", "AsmLine", "Pointer", "PoolEntry", "ChainedFixups", "FixupSegment",
    "find_llvm_objdump",
]

# ---------------------------------------------------------------------------
# Mach-O constants
# ---------------------------------------------------------------------------

MH_MAGIC_64 = 0xFEEDFACF
MH_CIGAM_64 = 0xCFFAEDFE

LC_SEGMENT = 0x01
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYSYMTAB = 0x0B
LC_UUID = 0x1B
LC_DYLD_CHAINED_FIXUPS = 0x80000034

CPU_TYPE_ARM64 = 0x0100000C

# chained pointer formats (see <mach-o/fixup-chains.h>)
DYLD_CHAINED_PTR_ARM64E = 1
DYLD_CHAINED_PTR_64 = 2
DYLD_CHAINED_PTR_32 = 3
DYLD_CHAINED_PTR_32_CACHE = 4
DYLD_CHAINED_PTR_32_FIRMWARE = 5
DYLD_CHAINED_PTR_64_OFFSET = 6
DYLD_CHAINED_PTR_ARM64E_OFFSET = 7
DYLD_CHAINED_PTR_ARM64E_KERNEL = 7
DYLD_CHAINED_PTR_64_KERNEL_CACHE = 8
DYLD_CHAINED_PTR_ARM64E_USERLAND = 9
DYLD_CHAINED_PTR_ARM64E_FIRMWARE = 10
DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE = 11
DYLD_CHAINED_PTR_ARM64E_USERLAND24 = 12
DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE = 13
DYLD_CHAINED_PTR_ARM64E_SEGMENTED = 14
DYLD_CHAINED_PTR_SHARED_CACHE_V2 = 16

PTR_FORMATS = {
    1: "arm64e (vmaddr)",
    2: "64 (vmaddr)",
    3: "32 (vmaddr)",
    4: "32 (cache)",
    5: "32 (firmware)",
    6: "64 (vm offset)",
    7: "arm64e (vm offset)",
    9: "arm64e userland (vm offset)",
    12: "arm64e userland24 (vm offset)",
    13: "arm64e shared cache",
    16: "shared cache v2",
}

# ObjC method list flags
METHOD_LIST_RELATIVE = 0x80000000


def _sx(value: int, bits: int) -> int:
    value &= (1 << bits) - 1
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


# ---------------------------------------------------------------------------
# data classes
# ---------------------------------------------------------------------------


@dataclass
class Section:
    segname: str
    sectname: str
    addr: int
    size: int
    offset: int
    align: int
    reloff: int
    nreloc: int
    flags: int
    reserved1: int
    reserved2: int
    index: int

    @property
    def name(self) -> str:
        return "%s,%s" % (self.segname, self.sectname)

    @property
    def end(self) -> int:
        return self.addr + self.size

    def contains(self, addr: int) -> bool:
        return self.addr <= addr < self.end

    def file_offset(self, addr: int) -> int:
        return self.offset + (addr - self.addr)

    def __str__(self) -> str:
        return "%-26s addr=%#012x size=%#08x off=%#08x" % (
            self.name, self.addr, self.size, self.offset)


@dataclass
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    maxprot: int
    initprot: int
    nsects: int
    flags: int
    index: int
    sections: List[Section] = field(default_factory=list)

    def contains(self, addr: int) -> bool:
        return self.vmaddr <= addr < self.vmaddr + self.vmsize

    def __str__(self) -> str:
        return "%-14s vmaddr=%#012x vmsize=%#010x fileoff=%#08x filesize=%#08x" % (
            self.name, self.vmaddr, self.vmsize, self.fileoff, self.filesize)


@dataclass
class Pointer:
    """A pointer read out of the image."""

    addr: int                 # where the pointer is stored
    raw: int                  # the raw 64 bit value
    kind: str                 # 'rebase' | 'bind' | 'plain' | 'unknown'
    target: Optional[int] = None    # resolved address (rebase/plain)
    ordinal: Optional[int] = None   # import ordinal (bind)
    name: Optional[str] = None      # import name (bind)

    def __str__(self) -> str:
        if self.kind == "bind":
            return "%#x -> bind %s" % (self.addr, self.name or self.ordinal)
        if self.target is not None:
            return "%#x -> %#x" % (self.addr, self.target)
        return "%#x -> ? (%#x)" % (self.addr, self.raw)


@dataclass
class CFString:
    addr: int          # address of the __CFString struct
    isa: int           # raw isa pointer
    flags: int         # raw 32 bit _cfinfo word
    cstring: int       # address of the UTF-8 text
    length: int
    text: str = ""

    def __str__(self) -> str:
        return "%#012x isa=%#x flags=%#x cstring=%#x len=%d %r" % (
            self.addr, self.isa, self.flags, self.cstring, self.length, self.text)


@dataclass
class PoolEntry:
    """One 8 byte slot of an array of CFString pointers."""

    slot: int          # address of the pointer
    pointer: Optional[Pointer] = None
    cfstring: Optional[CFString] = None

    @property
    def text(self) -> str:
        return self.cfstring.text if self.cfstring else ""

    def __str__(self) -> str:
        tgt = self.pointer.target if self.pointer else None
        return "%#012x -> %-14s %r" % (self.slot, hex(tgt) if tgt else "?", self.text)


@dataclass
class ObjCMethod:
    name: str
    types: str
    imp: int
    entry: int
    relative: bool
    is_class_method: bool = False
    owner: str = ""

    def __str__(self) -> str:
        kind = "+" if self.is_class_method else "-"
        return "%s[%s %s]  imp=%#x  types=%s" % (kind, self.owner, self.name,
                                                  self.imp, self.types)


@dataclass
class ObjCClass:
    name: str
    addr: int
    ro_addr: int
    flags: int
    instance_size: int
    superclass: Optional[str] = None
    methods: List[ObjCMethod] = field(default_factory=list)
    class_methods: List[ObjCMethod] = field(default_factory=list)
    ivars: List[str] = field(default_factory=list)

    def all_methods(self) -> List[ObjCMethod]:
        return self.methods + self.class_methods

    def __str__(self) -> str:
        return "class %s (%d methods, %d class methods)" % (
            self.name, len(self.methods), len(self.class_methods))


@dataclass
class AsmLine:
    addr: int
    raw: bytes
    text: str

    @property
    def word(self) -> int:
        return int.from_bytes(self.raw[:4], "little")


@dataclass
class Xref:
    addr: int
    kind: str          # 'adrp+add' | 'adrp+ldr' | ...
    target: int
    line: Optional[AsmLine] = None

    def __str__(self) -> str:
        txt = self.line.text if self.line else ""
        return "%#012x  %-10s %s" % (self.addr, self.kind, txt)


# ---------------------------------------------------------------------------
# chained fixups
# ---------------------------------------------------------------------------


@dataclass
class FixupSegment:
    seg_index: int
    page_size: int
    pointer_format: int
    segment_offset: int
    max_valid_pointer: int
    page_starts: List[int]


class ChainedFixups:
    """LC_DYLD_CHAINED_FIXUPS parser + pointer decoder.

    The pointer formats that matter for iOS user space binaries are

        2  DYLD_CHAINED_PTR_64          - 43 bit vm address
        6  DYLD_CHAINED_PTR_64_OFFSET   - 36 bit offset from the image base
        1/7/9/12 arm64e variants        - 43 bit address or 32 bit offset
    """

    def __init__(self, data: bytes, base: int, dataoff: int, datasize: int):
        self.data = data
        self.base = base
        self.dataoff = dataoff
        (self.version, self.starts_offset, self.imports_offset,
         self.symbols_offset, self.imports_count, self.imports_format,
         self.symbols_format) = struct.unpack_from("<7I", data, dataoff)
        self.segments: List[FixupSegment] = []
        self.imports: List[str] = []
        self._parse_starts()
        self._parse_imports()

    # -- parsing ----------------------------------------------------------
    def _parse_starts(self) -> None:
        off = self.dataoff + self.starts_offset
        seg_count, = struct.unpack_from("<I", self.data, off)
        offsets = struct.unpack_from("<%dI" % seg_count, self.data, off + 4)
        for idx, soff in enumerate(offsets):
            if soff == 0:
                continue
            p = off + soff
            size, page_size, pointer_format = struct.unpack_from("<IHH", self.data, p)
            segment_offset, max_valid_pointer = struct.unpack_from("<QI", self.data, p + 8)
            page_count, = struct.unpack_from("<H", self.data, p + 20)
            starts = list(struct.unpack_from("<%dH" % page_count, self.data, p + 22))
            self.segments.append(FixupSegment(idx, page_size, pointer_format,
                                              segment_offset, max_valid_pointer,
                                              starts))

    def _parse_imports(self) -> None:
        base = self.dataoff + self.imports_offset
        strings = self.dataoff + self.symbols_offset
        for i in range(self.imports_count):
            val, = struct.unpack_from("<I", self.data, base + 4 * i)
            if self.imports_format == 1:      # DYLD_CHAINED_IMPORT
                # lib_ordinal:8, weak_import:1, name_offset:23
                name_off = val & 0x7FFFFF
            else:
                name_off = val
            start = strings + name_off
            end = self.data.find(b"\0", start)
            if end < 0:
                end = start
            self.imports.append(self.data[start:end].decode("utf-8", "replace"))

    # -- queries ----------------------------------------------------------
    def import_name(self, ordinal: int) -> Optional[str]:
        if 0 <= ordinal < len(self.imports):
            return self.imports[ordinal]
        return None

    def segment_for(self, addr: int) -> Optional[FixupSegment]:
        for seg in self.segments:
            seg_start = self.base + seg.segment_offset
            seg_end = seg_start + len(seg.page_starts) * seg.page_size
            if seg_start <= addr < seg_end:
                return seg
        return None

    def covers(self, addr: int) -> bool:
        return self.segment_for(addr) is not None

    # -- decoding ---------------------------------------------------------
    def decode(self, addr: int, raw: int) -> Pointer:
        seg = self.segment_for(addr)
        if seg is None:
            return Pointer(addr, raw, "unknown")
        fmt = seg.pointer_format
        if fmt in (DYLD_CHAINED_PTR_64, DYLD_CHAINED_PTR_64_OFFSET):
            return self._decode_64(addr, raw, fmt)
        if fmt in (DYLD_CHAINED_PTR_ARM64E, DYLD_CHAINED_PTR_ARM64E_OFFSET,
                   DYLD_CHAINED_PTR_ARM64E_USERLAND, DYLD_CHAINED_PTR_ARM64E_USERLAND24,
                   DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE):
            return self._decode_arm64e(addr, raw, fmt)
        return Pointer(addr, raw, "unknown")

    def _decode_64(self, addr: int, raw: int, fmt: int) -> Pointer:
        # struct dyld_chained_ptr_64_rebase { target:36, high8:8, reserved:7,
        #                                       next:12, bind:1 }
        bind = (raw >> 63) & 1
        if bind:
            ordinal = raw & 0xFFFF
            return Pointer(addr, raw, "bind", ordinal=ordinal,
                           name=self.import_name(ordinal))
        if fmt == DYLD_CHAINED_PTR_64_OFFSET:
            target = raw & 0xFFFFFFFFF            # 36 bit offset
            high8 = (raw >> 36) & 0xFF
        else:
            target = raw & 0x7FFFFFFFFFF          # 43 bit vm address
            high8 = (raw >> 43) & 0xFF
        if high8:
            target |= high8 << 56
        elif fmt == DYLD_CHAINED_PTR_64_OFFSET:
            target += self.base
        return Pointer(addr, raw, "rebase", target=target)

    def _decode_arm64e(self, addr: int, raw: int, fmt: int) -> Pointer:
        bind = (raw >> 62) & 1
        auth = (raw >> 63) & 1
        if bind:
            ordinal = raw & 0xFFFF
            return Pointer(addr, raw, "bind", ordinal=ordinal,
                           name=self.import_name(ordinal))
        if auth:
            # dyld_chained_ptr_arm64e_auth_rebase: target:32 (runtime offset)
            target = raw & 0xFFFFFFFF
            return Pointer(addr, raw, "rebase", target=self.base + target)
        target = raw & 0x7FFFFFFFFFF               # 43 bit
        high8 = (raw >> 43) & 0xFF
        if high8:
            target |= high8 << 56
        elif fmt in (DYLD_CHAINED_PTR_ARM64E_OFFSET,
                     DYLD_CHAINED_PTR_ARM64E_USERLAND,
                     DYLD_CHAINED_PTR_ARM64E_USERLAND24):
            target += self.base
        return Pointer(addr, raw, "rebase", target=target)

    def __str__(self) -> str:
        out = ["chained fixups: %d imports, %d segments" % (
            len(self.imports), len(self.segments))]
        for seg in self.segments:
            out.append("  seg[%d] page=%#x format=%d (%s) offset=%#x pages=%d" % (
                seg.seg_index, seg.page_size, seg.pointer_format,
                PTR_FORMATS.get(seg.pointer_format, "?"), seg.segment_offset,
                len(seg.page_starts)))
        return "\n".join(out)


# ---------------------------------------------------------------------------
# llvm-objdump discovery
# ---------------------------------------------------------------------------

_OBJDUMP_LINE_RE = re.compile(r"^([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\t(.*)$")
_OBJDUMP_LINE_RE2 = re.compile(r"^([0-9a-f]+):\t([0-9a-f]{2}(?: [0-9a-f]{2})*)\t(.*)$")
_OTOOL_LINE_RE = re.compile(r"^([0-9a-f]{8,16})\t(.*)$")


def find_llvm_objdump() -> Optional[str]:
    """Locate an llvm-objdump that understands Mach-O."""
    env = os.environ.get("VENUS_LLVM_OBJDUMP")
    if env and os.path.exists(env):
        return env
    xcrun = shutil.which("xcrun")
    if xcrun:
        try:
            out = subprocess.run([xcrun, "-f", "llvm-objdump"], capture_output=True,
                                 text=True, timeout=20)
            if out.returncode == 0 and out.stdout.strip():
                return out.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    for pat in ("/Applications/Xcode.app/Contents/Developer/Toolchains/"
                "XcodeDefault.xctoolchain/usr/bin/llvm-objdump",
                "/usr/local/opt/llvm/bin/llvm-objdump",
                "/opt/homebrew/opt/llvm/bin/llvm-objdump"):
        if os.path.exists(pat):
            return pat
    for pat in ("/Applications/Xcode*.app/Contents/Developer/Toolchains/"
                "*.xctoolchain/usr/bin/llvm-objdump",):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    which = shutil.which("llvm-objdump")
    if which:
        return which
    return None


# ---------------------------------------------------------------------------
# the Mach-O reader
# ---------------------------------------------------------------------------


class MachO:
    def __init__(self, path: str, data: Optional[bytes] = None):
        self.path = path
        if data is None:
            with open(path, "rb") as fh:
                data = fh.read()
        self.data = data
        self.segments: List[Segment] = []
        self.sections: List[Section] = []
        self.fixups: Optional[ChainedFixups] = None
        self._disasm: Optional[List[AsmLine]] = None
        self._objdump_path: Optional[str] = None
        self._objdump_checked = False
        self._cfstrings: Optional[List[CFString]] = None
        self._classes: Optional[List[ObjCClass]] = None
        self._parse()

    # -- header / load commands -------------------------------------------
    def _parse(self) -> None:
        data = self.data
        magic, cputype, cpusub, filetype, ncmds, sizeofcmds, flags = \
            struct.unpack_from("<7I", data, 0)
        self.magic = magic
        self.cputype = cputype
        self.filetype = filetype
        self.ncmds = ncmds
        self.flags = flags
        if magic != MH_MAGIC_64:
            raise ValueError("%s: not a 64 bit little endian Mach-O (magic %#x)"
                             % (self.path, magic))
        off = 32
        seg_index = 0
        sect_index = 0
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", data, off)
            if cmd == LC_SEGMENT_64:
                name = data[off + 8:off + 24].rstrip(b"\0").decode("ascii", "replace")
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", data, off + 24)
                maxprot, initprot, nsects, sflags = struct.unpack_from("<4I", data, off + 56)
                seg = Segment(name, vmaddr, vmsize, fileoff, filesize, maxprot,
                              initprot, nsects, sflags, seg_index)
                so = off + 72
                for _s in range(nsects):
                    sectname = data[so:so + 16].rstrip(b"\0").decode("ascii", "replace")
                    segname = data[so + 16:so + 32].rstrip(b"\0").decode("ascii", "replace")
                    addr, size = struct.unpack_from("<QQ", data, so + 32)
                    foff, align, reloff, nreloc, sflags2, r1, r2 = \
                        struct.unpack_from("<7I", data, so + 48)
                    sect = Section(segname, sectname, addr, size, foff, align,
                                   reloff, nreloc, sflags2, r1, r2, sect_index)
                    seg.sections.append(sect)
                    self.sections.append(sect)
                    sect_index += 1
                    so += 80
                self.segments.append(seg)
                seg_index += 1
            elif cmd == LC_DYLD_CHAINED_FIXUPS:
                dataoff, datasize = struct.unpack_from("<II", data, off + 8)
                base = self.text_base()
                self.fixups = ChainedFixups(data, base, dataoff, datasize)
            off += cmdsize

    # -- basic accessors ---------------------------------------------------
    def text_base(self) -> int:
        """Lowest segment vmaddr (the base the chained fixups are relative to)."""
        bases = [s.vmaddr for s in self.segments if s.filesize]
        return min(bases) if bases else 0

    def section(self, sectname: str, segname: Optional[str] = None) -> Optional[Section]:
        for s in self.sections:
            if s.sectname == sectname and (segname is None or s.segname == segname):
                return s
        return None

    def section_at(self, addr: int) -> Optional[Section]:
        for s in self.sections:
            if s.contains(addr):
                return s
        return None

    def segment_at(self, addr: int) -> Optional[Segment]:
        for s in self.segments:
            if s.contains(addr):
                return s
        return None

    def vaddr_to_offset(self, addr: int) -> Optional[int]:
        for s in self.sections:
            if s.contains(addr):
                return s.file_offset(addr)
        seg = self.segment_at(addr)
        if seg is not None and seg.filesize:
            off = seg.fileoff + (addr - seg.vmaddr)
            if 0 <= off < len(self.data):
                return off
        return None

    def offset_to_vaddr(self, off: int) -> Optional[int]:
        for s in self.sections:
            if s.offset <= off < s.offset + s.size:
                return s.addr + (off - s.offset)
        return None

    def read(self, addr: int, size: int) -> bytes:
        off = self.vaddr_to_offset(addr)
        if off is None:
            raise ValueError("address %#x is not mapped" % addr)
        return self.data[off:off + size]

    def read_u64(self, addr: int) -> int:
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def cstring(self, addr: int, maxlen: int = 4096) -> str:
        off = self.vaddr_to_offset(addr)
        if off is None:
            return ""
        end = self.data.find(b"\0", off, off + maxlen)
        if end < 0:
            end = off + maxlen
        return self.data[off:end].decode("utf-8", "replace")

    def find_strings(self, needle: str, section: Optional[str] = "__cstring") -> List[Tuple[int, int]]:
        """Find a NUL terminated string; returns [(vaddr, file offset)]."""
        pat = needle.encode("utf-8") + b"\0"
        out = []
        secs = [s for s in self.sections if section is None or s.sectname == section]
        if not secs:
            secs = list(self.sections)
        for s in secs:
            if s.offset == 0 or s.size == 0:
                continue
            blob = self.data[s.offset:s.offset + s.size]
            pos = 0
            while True:
                idx = blob.find(pat, pos)
                if idx < 0:
                    break
                out.append((s.addr + idx, s.offset + idx))
                pos = idx + 1
        return out

    # -- pointers ----------------------------------------------------------
    def pointer(self, addr: int) -> Pointer:
        raw = self.read_u64(addr)
        if self.fixups is not None and self.fixups.covers(addr):
            ptr = self.fixups.decode(addr, raw)
            if ptr.kind != "unknown":
                return ptr
        # a plain (already bound) pointer?
        if self.section_at(raw) is not None:
            return Pointer(addr, raw, "plain", target=raw)
        if raw == 0:
            return Pointer(addr, raw, "plain", target=0)
        return Pointer(addr, raw, "unknown")

    # -- CFStrings ---------------------------------------------------------
    def cfstring_at(self, addr: int) -> Optional[CFString]:
        sect = self.section("__cfstring")
        if sect is None or not sect.contains(addr):
            return None
        raw = self.read(addr, 32)
        isa, flags, cstr, length = struct.unpack("<4Q", raw)
        cptr = self.pointer(addr + 16)
        caddr = cptr.target if cptr.target is not None else 0
        text = ""
        if caddr and length and length < 0x10000:
            text = self.read(caddr, length).decode("utf-8", "replace")
        return CFString(addr, isa, flags & 0xFFFFFFFF, caddr, length, text)

    def cfstrings(self) -> List[CFString]:
        """All CFString constants in the __cfstring section."""
        if self._cfstrings is not None:
            return self._cfstrings
        out: List[CFString] = []
        sect = self.section("__cfstring")
        if sect is not None:
            for off in range(sect.offset, sect.offset + sect.size, 32):
                addr = sect.addr + (off - sect.offset)
                cs = self.cfstring_at(addr)
                if cs is not None:
                    out.append(cs)
        self._cfstrings = out
        return out

    def pool_around(self, addr: int, count: int = 8) -> List[PoolEntry]:
        """The array of CFString pointers around ``addr``.

        The CFString constants of a binary usually live in __cfstring, but the
        code does not point at them directly: it points at an array of
        pointers to them, and that array is what shows up in the
        disassembly as ``adrp``/``add`` pairs.  This walks that array.
        """
        sect = self.section_at(addr)
        if sect is None:
            return []
        start = addr - (count // 2) * 8
        out = []
        for i in range(count):
            slot = start + i * 8
            if not sect.contains(slot):
                continue
            ptr = self.pointer(slot)
            cs = None
            if ptr.target is not None:
                cs = self.cfstring_at(ptr.target)
            out.append(PoolEntry(slot, ptr, cs))
        return out

    def cfstring_pool(self, addr: int, count: int = 8) -> List[PoolEntry]:
        """Like pool_around() but only the slots that really are CFStrings."""
        return [e for e in self.pool_around(addr, count) if e.cfstring is not None]

    def pool_slots_for(self, cfstring_addr: int) -> List[int]:
        """The addresses of the pointer slots that point at a CFString.

        The code of a binary normally does not reference a CFString object
        directly, it references an array of pointers to them; this finds the
        entries of those arrays.
        """
        out = []
        for sect in self.sections:
            if sect.sectname != "__const" or not sect.offset or sect.size < 8:
                continue
            for i in range(0, sect.size - 7, 8):
                slot = sect.addr + i
                ptr = self.pointer(slot)
                if ptr.kind == "rebase" and ptr.target == cfstring_addr:
                    out.append(slot)
        return out

    # -- ObjC --------------------------------------------------------------
    def _ptr(self, field_addr: int, raw: Optional[int] = None) -> Optional[int]:
        """Resolve the pointer stored at ``field_addr`` (chained fixup aware)."""
        if raw is None:
            raw = self.read_u64(field_addr)
        if self.fixups is not None and self.fixups.covers(field_addr):
            p = self.fixups.decode(field_addr, raw)
            if p.kind == "rebase" and p.target is not None:
                return p.target
            if p.kind == "plain" and p.target is not None:
                return p.target
        if self.section_at(raw) is not None:
            return raw
        return None

    def _rel_ptr(self, field_addr: int, raw: int) -> Optional[int]:
        """Some fields are stored relative to their own address."""
        rel = _sx(raw, 64)
        if self.section_at(field_addr + rel) is not None:
            return field_addr + rel
        return None

    def _resolve_name(self, addr: int, field_addr: int) -> str:
        """Resolve a char* that may be stored absolute or relative."""
        raw = self.read_u64(field_addr)
        target = self._ptr(field_addr, raw)
        if target is None:
            target = self._rel_ptr(field_addr, raw)
        if target is None:
            return ""
        return self.cstring(target)

    def _selector(self, addr: int) -> str:
        """A selector may be a direct string or a pointer to a selref."""
        sect = self.section_at(addr)
        if sect is not None and sect.sectname == "__objc_methname":
            return self.cstring(addr)
        if sect is not None and sect.sectname in ("__objc_selrefs", "__objc_const"):
            ptr = self.pointer(addr)
            if ptr.target is not None:
                return self.cstring(ptr.target)
        raw = self.read_u64(addr)
        if self.section_at(raw) is not None:
            return self.cstring(raw)
        return ""

    def _method_list(self, ml_addr: int, owner: str,
                     is_class_method: bool) -> List[ObjCMethod]:
        if not ml_addr or ml_addr == self.text_base():
            return []
        try:
            entsize, count = struct.unpack("<II", self.read(ml_addr, 8))
        except (ValueError, struct.error):
            return []
        relative = bool(entsize & METHOD_LIST_RELATIVE)
        stride = (entsize & 0xFFFF) or 12
        out = []
        for i in range(count):
            entry = ml_addr + 8 + i * stride
            try:
                if relative:
                    name_off, types_off, imp_off = struct.unpack("<iii", self.read(entry, 12))
                    name_addr = entry + name_off
                    types_addr = entry + 4 + types_off
                    imp = entry + 8 + imp_off
                else:
                    name_addr = self._ptr(entry)
                    types_addr = self._ptr(entry + 8)
                    imp = self._ptr(entry + 16)
            except (ValueError, struct.error):
                break
            name = self._selector(name_addr) if name_addr else ""
            types = self.cstring(types_addr) if types_addr else ""
            out.append(ObjCMethod(name, types, imp, entry, relative,
                                   is_class_method, owner))
        return out

    def _class_ro(self, cls_addr: int) -> Dict:
        """Parse class_t -> class_ro_t."""
        if not cls_addr or cls_addr == self.text_base():
            raise ValueError("no class at %#x" % cls_addr)
        raw = self.read(cls_addr, 40)
        isa_raw, super_raw, cache, vtable, data_raw = struct.unpack("<5Q", raw)
        isa = self._ptr(cls_addr, isa_raw)
        superc = self._ptr(cls_addr + 8, super_raw)
        data = self._ptr(cls_addr + 32, data_raw)
        if data is None:
            data = data_raw & ~0x7
        ro_addr = data & ~0x7
        if self.section_at(ro_addr) is None:
            raise ValueError("class_ro_t %#x is not mapped" % ro_addr)
        ro = self.read(ro_addr, 64)
        flags, instance_start, instance_size, _reserved = struct.unpack_from("<4I", ro, 0)
        ivar_layout_raw, name_raw, methods_raw = struct.unpack_from("<3Q", ro, 16)
        ivar_layout = self._ptr(ro_addr + 16, ivar_layout_raw)
        name = self._resolve_name(0, ro_addr + 24)
        base_methods = self._ptr(ro_addr + 32, methods_raw)
        if base_methods is None:
            base_methods = self._rel_ptr(ro_addr + 32, methods_raw)
        return {
            "isa": isa,
            "superclass": superc,
            "data": data,
            "ro": ro_addr,
            "flags": flags,
            "instance_start": instance_start,
            "instance_size": instance_size,
            "name": name,
            "base_methods": base_methods,
            "ivar_layout": ivar_layout,
        }

    def objc_classes(self) -> List[ObjCClass]:
        if self._classes is not None:
            return self._classes
        out: List[ObjCClass] = []
        sect = self.section("__objc_classlist")
        if sect is None:
            self._classes = out
            return out
        n = sect.size // 8
        for i in range(n):
            slot = sect.addr + i * 8
            ptr = self.pointer(slot)
            if not ptr.target:
                continue
            info = self._class_ro(ptr.target)
            name = info["name"] or ("<class@%#x>" % ptr.target)
            cls = ObjCClass(name, ptr.target, info["ro"], info["flags"],
                            info["instance_size"])
            cls.methods = self._method_list(info["base_methods"], name, False)
            # the metaclass (class_t.isa) holds the class methods
            meta = None
            if info["isa"]:
                try:
                    meta = self._class_ro(info["isa"])
                except (ValueError, struct.error):
                    meta = None
            if meta and meta["base_methods"] and meta["base_methods"] != self.text_base():
                cls.class_methods = self._method_list(meta["base_methods"], name, True)
            # superclass name
            if info["superclass"]:
                sp = self.pointer(info["superclass"])
                if sp.target:
                    try:
                        cls.superclass = self._class_ro(sp.target)["name"] or None
                    except (ValueError, struct.error):
                        pass
            out.append(cls)
        self._classes = out
        return out

    def objc_class_named(self, name: str) -> Optional[ObjCClass]:
        for c in self.objc_classes():
            if c.name == name:
                return c
        return None

    def objc_classrefs(self) -> List[Tuple[int, str]]:
        """[(address, class name)] of the __objc_classrefs (with the comment
        llvm-objdump prints, e.g. 'Objc class ref: MALog')."""
        out = []
        sect = self.section("__objc_classrefs")
        if sect is None:
            return out
        for i in range(sect.size // 8):
            slot = sect.addr + i * 8
            ptr = self.pointer(slot)
            if not ptr.target:
                continue
            try:
                info = self._class_ro(ptr.target)
            except (ValueError, struct.error):
                continue
            out.append((slot, info["name"]))
        return out

    # -- disassembly -------------------------------------------------------
    def objdump(self) -> Optional[str]:
        if not self._objdump_checked:
            self._objdump_path = find_llvm_objdump()
            self._objdump_checked = True
        return self._objdump_path

    def _run_objdump(self) -> List[AsmLine]:
        path = self.objdump()
        if path is None:
            return []
        cmd = [path, "--macho", "--disassemble", self.path]
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        except (OSError, subprocess.SubprocessError):
            return []
        lines: List[AsmLine] = []
        for line in out.stdout.splitlines():
            m = _OBJDUMP_LINE_RE.match(line)
            if m:
                raw = bytes.fromhex(m.group(2).replace(" ", ""))
                lines.append(AsmLine(int(m.group(1), 16), raw, m.group(3)))
                continue
            m = _OBJDUMP_LINE_RE2.match(line)
            if m:
                raw = bytes.fromhex(m.group(2).replace(" ", ""))
                lines.append(AsmLine(int(m.group(1), 16), raw, m.group(3)))
        return lines

    def _run_otool(self) -> List[AsmLine]:
        otool = shutil.which("otool")
        if otool is None:
            return []
        try:
            out = subprocess.run([otool, "-tvV", self.path], capture_output=True,
                                 text=True, timeout=900)
        except (OSError, subprocess.SubprocessError):
            return []
        lines: List[AsmLine] = []
        for line in out.stdout.splitlines():
            m = _OTOOL_LINE_RE.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            off = self.vaddr_to_offset(addr)
            if off is None:
                continue
            raw = self.data[off:off + 4]
            lines.append(AsmLine(addr, raw, m.group(2)))
        return lines

    def disassemble(self, start: Optional[int] = None,
                    stop: Optional[int] = None) -> List[AsmLine]:
        """Disassemble (a range of) the text section.

        llvm-objdump ignores --start-address/--stop-address for Mach-O, so the
        whole __text is disassembled once and cached, then filtered.
        """
        if self._disasm is None:
            lines = self._run_objdump()
            if not lines:
                lines = self._run_otool()
            self._disasm = lines
        out = self._disasm
        if start is not None:
            out = [l for l in out if l.addr >= start]
        if stop is not None:
            out = [l for l in out if l.addr < stop]
        return out

    def xrefs(self, target: int, start: Optional[int] = None,
              stop: Optional[int] = None) -> List[Xref]:
        """Find the adrp+add (and adrp+ldr) references to ``target``."""
        regs: Dict[int, int] = {}
        out: List[Xref] = []
        for line in self.disassemble(start, stop):
            try:
                ins = asm.decode(line.word, pc=line.addr)
            except ValueError:
                continue
            m = ins.mnemonic
            if m == "adrp":
                regs[ins.fields["rd"]] = ins.fields["page"]
                continue
            if m == "add" and ins.fields.get("rn") in regs:
                val = regs[ins.fields["rn"]] + (ins.fields["imm12"] << ins.fields["shift"])
                if val == target:
                    out.append(Xref(line.addr, "adrp+add", val, line))
                regs[ins.fields["rd"]] = val
                continue
            if m in ("ldr", "str") and ins.fields.get("rn") in regs:
                val = regs[ins.fields["rn"]] + ins.fields["imm"]
                if val == target:
                    out.append(Xref(line.addr, "adrp+" + m, val, line))
                if m == "ldr":
                    regs.pop(ins.fields["rt"], None)
                continue
            if ins.dest is not None:
                regs.pop(ins.dest, None)
        return out

    def xrefs_to_string(self, text: str) -> List[Tuple[int, int, List[Xref]]]:
        """[(cstring vaddr, file offset, xrefs)] for a string in __cstring."""
        out = []
        for addr, off in self.find_strings(text):
            out.append((addr, off, self.xrefs(addr)))
        return out

    def __str__(self) -> str:
        out = ["%s (%d bytes)" % (self.path, len(self.data))]
        for seg in self.segments:
            out.append(str(seg))
        return "\n".join(out)
