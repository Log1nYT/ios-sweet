#!/bin/bash
# Build the venus (SM8350) shim on macOS with the Mu-Silicium clang toolchain.
# Linux equivalent: CFLAGS as below + aarch64-linux-gnu-gcc, mkbootimg in tools/.
set -euo pipefail
cd "$(dirname "$0")"

# The Mu-Silicium toolchain was removed to free disk space; Homebrew llvm + lld do the same job
# (brew install llvm lld).  TC may still point at the old directory.
TC=${TC:-/opt/homebrew/opt/llvm/bin}
CC=${CC:-$TC/clang}
LD=${LD:-/opt/homebrew/bin/ld.lld}
OBJCOPY=${OBJCOPY:-$TC/llvm-objcopy}
NM=${NM:-$TC/llvm-nm}
MKBOOTIMG=${MKBOOTIMG:-../tools/mkbootimg.py}
DTB=${DTB:-$(pwd)/venus.dtb}   # the phone's device tree (extracted from the known-good kernel blob)

TARGET=aarch64-none-elf
CFLAGS="--target=$TARGET -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
LDFLAGS="-nostdlib -static -T shim.ld --build-id=none -N"

mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done

$LD $LDFLAGS \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$($NM out/shim.elf | awk '$3=="_end"{print $1}' | python3 -c "import sys; print(int(sys.stdin.read().strip(), 16))")
truncate -s "$size" out/Image

# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else 5 bytes
# of "dummy" (the shape Xiaomi ABL provably accepts -> shim-only diagnostics).
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	printf 'dummy' > out/ramdisk.bin
fi

# venus boot image, exactly the Mu-recipe shape this ABL provably boots:
# gzip(Image) with the RAW dtb appended after the stream, ramdisk "dummy".
# (A raw Image+dtb-in-gzip blob gave one transient "Load Error"; never since.)
gzip -c out/Image > out/Image.gz
cat out/Image.gz "$DTB" > out/kernel_blob.bin
python3 "$MKBOOTIMG" --kernel out/kernel_blob.bin --ramdisk out/ramdisk.bin \
	-o out/shim-boot-venus.img --header_version 1

echo "built:"
ls -l out/Image out/shim-boot-venus.img
