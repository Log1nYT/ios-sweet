# UEFI for venus (Mi 11) with the HackiOS XNU loader hook

Our edits to the local Mu-Silicium tree (`~/Documents/Mu-Silicium`, not a git tree) that make the
venus UEFI start the donor's resident loader (github.com/oosexgang/hackios-xnu-kernel):

- `BootManager.c`: `#include "XnuLoader.h"` + `XnuLoaderMain()` in `PlatformBootManagerPriorityBoot`
  (no Volume Up) and in `UnableToBoot`. The donor's `XnuLoader.c/.h` and the `BootShim.S` /
  `BootManagerLib.inf` parts of `uefi/mu-silicium.patch` are theirs (APSL 2.0), not copied here.
- `venus.fdf`/`venus.dsc`/`Include/APRIORI.inc`: the old `IosLoader` DXE driver is commented out
  (it halted the boot looking for `kern.raw` on an ESP).
- `MemoryMapLib.c`: `XNU Payload` region at 0xF0000000 (without it UEFI faults reading the payload).
- `build_uefi.py`: patches are applied with patch(1) (the tree has no usable .git; `git apply` failed).
Build: `./build-macos.sh -r RELEASE` in Mu-Silicium, then `mkxnuimg.py` of the donor
(`UEFI_TARGET=venus`, `FD_BASE=0x9FD00000`, `FD_SIZE=0x200000`, `PAYLOAD_BASE=0xF0000000`,
`PATH` with Homebrew llvm for `llvm-objcopy`).
