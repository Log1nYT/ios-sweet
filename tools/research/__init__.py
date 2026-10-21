"""venus reverse-engineering toolkit.

Small, dependency-free (python3 stdlib only) helpers for looking at iOS
binaries on the Mac:

  * ``bplist``  - binary property list (bplist00) reader with byte offsets,
                  so a value can be patched in place.
  * ``macho``   - arm64 Mach-O reader: segments/sections, vaddr<->offset,
                  chained fixups, the CFString pool, ObjC classes/methods,
                  disassembly and adrp/add xref search.
  * ``asm``     - arm64 encode/decode for the handful of instructions we
                  need when computing and verifying patches.
  * ``rtool``   - tiny command line front end for all of the above.

Nothing in here touches the device; everything works on files on disk.
"""

__all__ = ["asm", "bplist", "macho"]
__version__ = "1.0"
