"""arm64 (A64) instruction encode/decode for the instructions venus needs.

Only a small subset is implemented, but it is exactly the subset that shows up
when we compute and verify byte patches in iOS binaries:

  * ``ldr`` / ``str``  - immediate, unsigned offset, 12 bit scaled imm
  * ``movz`` / ``movn`` / ``movk`` (and the ``mov`` alias)
  * ``adr`` / ``adrp``
  * ``b`` / ``bl``
  * ``b.cond``
  * ``cbz`` / ``cbnz``
  * ``tbz`` / ``tbnz``
  * ``add`` / ``sub`` (immediate) - needed for the adrp+add xref scan
  * ``nop``

All encodings were checked against ``llvm-objdump`` output of
``logs/mobileactivationd-20H392.bin``.

Example
-------
>>> w = 0xf9450503                     # ldr x3, [x8, #0xa08]
>>> i = decode(w)
>>> i.mnemonic
'ldr'
>>> i.fields['imm']
0xa08
>>> hex(encode('ldr', rt=3, rn=8, imm=0xa10, size=3, load=True))
'0xf9450903'
"""

from __future__ import annotations

from typing import Dict, Optional

__all__ = [
    "Insn",
    "decode",
    "encode",
    "format_insn",
    "branch_target",
    "retarget",
    "CONDITIONS",
    "REG_NAMES",
]

MASK64 = (1 << 64) - 1

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def _sx(value: int, bits: int) -> int:
    """Sign extend ``value`` (interpreted as ``bits`` wide) to a python int."""
    value &= (1 << bits) - 1
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def _uns(value: int, bits: int) -> int:
    return value & ((1 << bits) - 1)


def _reg_name(num: int, sf: int = 1) -> str:
    if num == 31:
        return "sp" if sf else "wsp"
    return ("x%d" if sf else "w%d") % num


def _parse_reg_name(name) -> tuple:
    """``"x8"`` -> ``(8, 1)``, ``"w3"`` -> ``(3, 0)``, ``"sp"`` -> ``(31, 1)``."""
    if isinstance(name, int):
        return (name, 1)
    name = str(name).strip().lower()
    if name in ("sp", "wsp"):
        return (31, 0 if name == "wsp" else 1)
    if name.startswith("x"):
        return (int(name[1:]), 1)
    if name.startswith("w"):
        return (int(name[1:]), 0)
    raise ValueError("bad register name %r" % (name,))


CONDITIONS = {
    0x0: "eq", 0x1: "ne", 0x2: "cs", 0x3: "cc", 0x4: "mi", 0x5: "pl",
    0x6: "vs", 0x7: "vc", 0x8: "hi", 0x9: "ls", 0xa: "ge", 0xb: "lt",
    0xc: "gt", 0xd: "le", 0xe: "al", 0xf: "nv",
}

REG_NAMES = ("x%d" % i for i in range(32))


# ---------------------------------------------------------------------------
# the instruction record
# ---------------------------------------------------------------------------


class Insn:
    """A decoded instruction.

    ``fields`` holds the decoded operands, e.g. ``{'rt': 3, 'rn': 8,
    'imm': 0xa08, 'size': 3, 'load': True}``.
    """

    __slots__ = ("word", "mnemonic", "fields", "dest")

    def __init__(self, word: int, mnemonic: str, fields: Dict, dest: Optional[int] = None):
        self.word = word & MASK64
        self.mnemonic = mnemonic
        self.fields = fields
        self.dest = dest

    def __getitem__(self, key):
        return self.fields[key]

    def get(self, key, default=None):
        return self.fields.get(key, default)

    def __contains__(self, key):
        return key in self.fields

    def __repr__(self):  # pragma: no cover - debug helper
        return "<Insn %#010x %s>" % (self.word, format_insn(self))

    def __str__(self):  # pragma: no cover - debug helper
        return format_insn(self)


def format_insn(insn: Insn) -> str:
    """Render an instruction roughly the way llvm-objdump does."""
    m = insn.mnemonic
    f = insn.fields
    if m in ("ldr", "str"):
        rt = _reg_name(f["rt"], f["size"] == 3)
        rn = _reg_name(f["rn"], f["size"] == 3)
        return "%s\t%s, [%s, #%#x]" % (m, rt, rn, f["imm"])
    if m in ("movz", "movn", "movk"):
        rd = _reg_name(f["rd"], f["sf"])
        if f.get("shift"):
            return "%s\t%s, #%#x, lsl #%d" % (m, rd, f["imm16"], f["shift"])
        return "%s\t%s, #%#x" % (m, rd, f["imm16"])
    if m == "adrp":
        return "adrp\t%s, %#x" % (_reg_name(f["rd"], 1), f["page"])
    if m == "adr":
        return "adr\t%s, %#x" % (_reg_name(f["rd"], 1), f["target"])
    if m in ("b", "bl"):
        return "%s\t%#x" % (m, f["target"])
    if m == "b.cond":
        return "b.%s\t%#x" % (f["cond"], f["target"])
    if m in ("cbz", "cbnz"):
        return "%s\t%s, %#x" % (m, _reg_name(f["rt"], f["sf"]), f["target"])
    if m in ("tbz", "tbnz"):
        return "%s\t%s, #%d, %#x" % (m, _reg_name(f["rt"], 0), f["bit"], f["target"])
    if m in ("add", "sub"):
        rd = _reg_name(f["rd"], f["sf"])
        rn = _reg_name(f["rn"], f["sf"])
        if f.get("shift"):
            return "%s\t%s, %s, #%#x, lsl #%d" % (m, rd, rn, f["imm12"], f["shift"] * 12)
        return "%s\t%s, %s, #%#x" % (m, rd, rn, f["imm12"])
    if m == "nop":
        return "nop"
    return m


# ---------------------------------------------------------------------------
# decoder
# ---------------------------------------------------------------------------


def decode(word: int, pc: Optional[int] = None) -> Insn:
    """Decode a 32 bit arm64 instruction word.

    ``pc`` (the address of the instruction) is optional; when given, branch
    targets are computed as absolute addresses.
    """
    word &= 0xFFFFFFFF

    # nop (hint #0)
    if word == 0xD503201F:
        return Insn(word, "nop", {})

    # B / BL :  op(31) 100101 imm26
    if (word & 0x7C000000) == 0x14000000:
        imm26 = _sx(word & 0x03FFFFFF, 26)
        target = None if pc is None else pc + imm26 * 4
        name = "bl" if (word >> 31) & 1 else "b"
        return Insn(word, name, {"imm26": imm26, "target": target}, dest=None)

    # B.cond :  0101010 0 imm19 0 cond
    if (word & 0xFF000010) == 0x54000000:
        imm19 = _sx((word >> 5) & 0x7FFFF, 19)
        target = None if pc is None else pc + imm19 * 4
        cond = word & 0xF
        return Insn(word, "b.cond",
                    {"imm19": imm19, "cond": CONDITIONS[cond], "cond_num": cond,
                     "target": target})

    # CBZ / CBNZ :  sf 011010 op imm19 Rt
    if (word & 0x7E000000) == 0x34000000:
        sf = (word >> 31) & 1
        op = (word >> 24) & 1
        imm19 = _sx((word >> 5) & 0x7FFFF, 19)
        target = None if pc is None else pc + imm19 * 4
        rt = word & 0x1F
        name = "cbnz" if op else "cbz"
        return Insn(word, name,
                    {"sf": sf, "rt": rt, "imm19": imm19, "target": target})

    # TBZ / TBNZ :  b5(31) 011011(30:25) op(24) bit40(23:19) imm14(18:5) Rt(4:0)
    if (word & 0x7E000000) == 0x36000000:
        b5 = (word >> 31) & 1
        op = (word >> 24) & 1
        bit = (b5 << 5) | ((word >> 19) & 0x1F)
        imm14 = _sx((word >> 5) & 0x3FFF, 14)
        target = None if pc is None else pc + imm14 * 4
        rt = word & 0x1F
        name = "tbnz" if op else "tbz"
        return Insn(word, name,
                    {"rt": rt, "bit": bit, "imm14": imm14, "target": target})

    # ADR / ADRP :  op 0 immlo 10000 immhi Rd
    if (word & 0x1F000000) == 0x10000000:
        op = (word >> 31) & 1
        immlo = (word >> 29) & 3
        immhi = (word >> 5) & 0x7FFFF
        imm21 = _sx((immhi << 2) | immlo, 21)
        rd = word & 0x1F
        if op:
            page = None if pc is None else (pc & ~0xFFF) + (imm21 << 12)
            return Insn(word, "adrp",
                        {"rd": rd, "imm21": imm21, "page": page}, dest=rd)
        target = None if pc is None else pc + imm21
        return Insn(word, "adr", {"rd": rd, "imm21": imm21, "target": target}, dest=rd)

    # MOVN / MOVZ / MOVK :  sf opc(30:29) 100101(28:23) hw(22:21) imm16(20:5) Rd
    if (word & 0x1F800000) == 0x12800000:
        sf = (word >> 31) & 1
        opc = (word >> 29) & 3
        hw = (word >> 21) & 3
        imm16 = (word >> 5) & 0xFFFF
        rd = word & 0x1F
        name = {0: "movn", 2: "movz", 3: "movk"}.get(opc)
        if name is None:
            raise ValueError("unsupported wide-move opc %d" % opc)
        return Insn(word, name,
                    {"sf": sf, "rd": rd, "imm16": imm16, "shift": hw * 16,
                     "opc": opc}, dest=rd)

    # ADD / SUB (immediate) :  sf op S 10001 sh imm12 Rn Rd
    if (word & 0x1F000000) == 0x11000000:
        sf = (word >> 31) & 1
        op = (word >> 30) & 1
        s = (word >> 29) & 1
        sh = (word >> 22) & 1
        imm12 = (word >> 10) & 0xFFF
        rn = (word >> 5) & 0x1F
        rd = word & 0x1F
        name = "sub" if op else "add"
        if s:
            name += "s"
        return Insn(word, name,
                    {"sf": sf, "rd": rd, "rn": rn, "imm12": imm12,
                     "shift": sh * 12, "setflags": s},
                    dest=rd if not s else rd)

    # LDR / STR (immediate, unsigned offset) :  size 111 V 01 opc imm12(21:10) Rn Rt
    # (bits 25:24 == 01 is the "unsigned offset" form; 00 is LDUR / pre / post index)
    if (word & 0x3F000000) == 0x39000000:
        size = (word >> 30) & 3
        v = (word >> 26) & 1
        opc = (word >> 22) & 3
        imm12 = (word >> 10) & 0xFFF
        rn = (word >> 5) & 0x1F
        rt = word & 0x1F
        if v == 0 and opc in (0, 1):
            name = "ldr" if opc else "str"
            return Insn(word, name,
                        {"size": size, "rt": rt, "rn": rn, "imm": imm12 << size,
                         "imm12": imm12, "load": opc == 1, "v": v},
                        dest=rt if opc else None)
        raise ValueError("unsupported load/store variant %#010x" % word)

    raise ValueError("unknown instruction %#010x" % word)


# ---------------------------------------------------------------------------
# encoder
# ---------------------------------------------------------------------------


def encode(mnemonic: str, **kw) -> int:
    """Encode an instruction; returns the 32 bit word.

    Supported keywords depend on the mnemonic:

    ``ldr``/``str``   rt, rn, imm (byte offset), size (0..3), load (bool)
    ``movz``/``movn``/``movk``  rd, imm16, shift (0/16/32/48), sf
    ``adrp``/``adr``  rd, imm21  (or ``page``/``target`` with ``pc``)
    ``b``/``bl``      imm26 (or ``target`` with ``pc``)
    ``b.cond``        cond, imm19 (or ``target`` with ``pc``)
    ``cbz``/``cbnz``  rt, imm19 (or ``target`` with ``pc``)
    ``tbz``/``tbnz``  rt, bit, imm14 (or ``target`` with ``pc``)
    ``add``/``sub``   rd, rn, imm12, shift (0/12), sf
    ``nop``
    """
    m = mnemonic.lower()

    if m in ("ldr", "str"):
        rt, rn = _parse_reg_name(kw["rt"])[0], _parse_reg_name(kw["rn"])[0]
        size = kw.get("size", 3)
        imm = kw.get("imm", 0)
        if imm & ((1 << size) - 1):
            raise ValueError("immediate %#x is not aligned to %d bytes" % (imm, 1 << size))
        imm12 = imm >> size
        if not 0 <= imm12 <= 0xFFF:
            raise ValueError("immediate out of range")
        load = kw.get("load", m == "ldr")
        opc = 1 if load else 0
        word = (size << 30) | 0x39000000 | (opc << 22) | (imm12 << 10) | (rn << 5) | rt
        return word

    if m in ("movz", "movn", "movk"):
        rd = _parse_reg_name(kw["rd"])[0]
        sf = kw.get("sf", 1)
        opc = {"movn": 0, "movz": 2, "movk": 3}[m]
        imm16 = kw.get("imm16", 0) & 0xFFFF
        shift = kw.get("shift", 0)
        if shift % 16 or shift not in (0, 16, 32, 48):
            raise ValueError("bad shift %r" % (shift,))
        hw = shift // 16
        return (0x12800000 | (sf << 31) | (opc << 29) | (hw << 21) |
                (imm16 << 5) | rd)

    if m in ("adrp", "adr"):
        rd = _parse_reg_name(kw["rd"])[0]
        if "imm21" in kw:
            imm21 = _sx(kw["imm21"], 21)
        else:
            pc = kw["pc"]
            if m == "adrp":
                delta = (kw["page"] - (pc & ~0xFFF)) >> 12
            else:
                delta = kw["target"] - pc
            imm21 = _sx(delta, 21)
        immlo = imm21 & 3
        immhi = (imm21 >> 2) & 0x7FFFF
        op = 1 if m == "adrp" else 0
        return (op << 31) | (immlo << 29) | 0x10000000 | (immhi << 5) | rd

    if m in ("b", "bl"):
        if "imm26" in kw:
            imm26 = _sx(kw["imm26"], 26)
        else:
            imm26 = _sx((kw["target"] - kw["pc"]) // 4, 26)
        op = 1 if m == "bl" else 0
        return (op << 31) | 0x14000000 | (imm26 & 0x03FFFFFF)

    if m == "b.cond":
        cond = kw["cond"]
        if isinstance(cond, str):
            cond = [k for k, v in CONDITIONS.items() if v == cond][0]
        if "imm19" in kw:
            imm19 = _sx(kw["imm19"], 19)
        else:
            imm19 = _sx((kw["target"] - kw["pc"]) // 4, 19)
        return 0x54000000 | ((imm19 & 0x7FFFF) << 5) | (cond & 0xF)

    if m in ("cbz", "cbnz"):
        rt = _parse_reg_name(kw["rt"])[0]
        sf = kw.get("sf", 1)
        op = 1 if m == "cbnz" else 0
        if "imm19" in kw:
            imm19 = _sx(kw["imm19"], 19)
        else:
            imm19 = _sx((kw["target"] - kw["pc"]) // 4, 19)
        return (sf << 31) | 0x34000000 | (op << 24) | ((imm19 & 0x7FFFF) << 5) | rt

    if m in ("tbz", "tbnz"):
        rt = _parse_reg_name(kw["rt"])[0]
        bit = kw["bit"]
        b5 = (bit >> 5) & 1
        bit40 = bit & 0x1F
        if "imm14" in kw:
            imm14 = _sx(kw["imm14"], 14)
        else:
            imm14 = _sx((kw["target"] - kw["pc"]) // 4, 14)
        op = 1 if m == "tbnz" else 0
        return ((b5 << 31) | 0x36000000 | (op << 24) | (bit40 << 19) |
                ((imm14 & 0x3FFF) << 5) | (rt & 0x1F))

    if m in ("add", "sub"):
        rd = _parse_reg_name(kw["rd"])[0]
        rn = _parse_reg_name(kw["rn"])[0]
        sf = kw.get("sf", 1)
        op = 1 if m == "sub" else 0
        imm12 = kw.get("imm12", 0) & 0xFFF
        shift = kw.get("shift", 0)
        if shift not in (0, 12):
            raise ValueError("bad shift %r" % (shift,))
        return (sf << 31) | (op << 30) | 0x11000000 | ((shift // 12) << 22) | \
            (imm12 << 10) | (rn << 5) | rd

    if m == "nop":
        return 0xD503201F

    raise ValueError("cannot encode %r" % (mnemonic,))


def branch_target(word: int, pc: int) -> Optional[int]:
    """Absolute target of a branch-like instruction, or None."""
    try:
        insn = decode(word, pc=pc)
    except ValueError:
        return None
    if insn.mnemonic in ("b", "bl", "b.cond", "cbz", "cbnz", "tbz", "tbnz"):
        return insn.fields["target"]
    return None


def retarget(word: int, pc: int, target: int) -> int:
    """Return ``word`` re-encoded so that it branches to ``target``."""
    insn = decode(word, pc=pc)
    m = insn.mnemonic
    if m in ("b", "bl"):
        return encode(m, target=target, pc=pc)
    if m == "b.cond":
        return encode(m, cond=insn.fields["cond_num"], target=target, pc=pc)
    if m in ("cbz", "cbnz"):
        return encode(m, rt=insn.fields["rt"], sf=insn.fields["sf"],
                      target=target, pc=pc)
    if m in ("tbz", "tbnz"):
        return encode(m, rt=insn.fields["rt"] & 0x1F, bit=insn.fields["bit"],
                      target=target, pc=pc)
    raise ValueError("%s is not a branch" % m)
