#!/bin/bash
# macOS twin of tools/mkiosroot.sh: build the SWEET iOS root as one
# case-sensitive journaled HFS+ raw image for the userdata partition.
#
#   sudo tools/mkiosroot-mac.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE_MIB]
#
# SIZE_MIB defaults to 229975 (the venus userdata partition, never smaller:
# fastboot erases the whole partition before it writes). The file is sparse,
# only the used ~13 GiB hit the disk. Nothing is flashed here.
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,10p' "$0"; exit 2; }
[[ $(id -u) == 0 ]] || { echo "run with sudo" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
sysos=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
appos=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
out=$4
mib=${5:-229975}
if [[ ${RESUME:-0} == 1 ]]; then
	[[ -e $out ]] || { echo "RESUME=1 but $out does not exist" >&2; exit 1; }
else
	[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
fi

work=$(mktemp -d /tmp/iosroot.XXXXXX)
mkdir "$work"/{os,sysos,appos,root}
devs=()
cleanup() {
	set +e
	for m in root appos sysos os; do
		mount | grep -q " $work/$m " && umount "$work/$m"
	done
	for d in ${devs[@]+"${devs[@]}"}; do hdiutil detach "$d" >/dev/null 2>&1; done
	rm -f "$work"/systembag.kb; rmdir "$work"/{os,sysos,appos,root} "$work" 2>/dev/null
}
trap cleanup EXIT

attach_ro() {	# file mountpoint [raw]
	local extra=()
	[[ ${3:-} == raw ]] && extra=(-imagekey diskimage-class=CRawDiskImage)
	local info
	info=$(hdiutil attach -readonly -nobrowse -noverify ${extra[@]+"${extra[@]}"} -mountpoint "$2" "$1")
	devs+=("$(echo "$info" | head -1 | awk '{print $1}')")
}
if [[ ${RESUME:-0} != 1 ]]; then
attach_ro "$os" "$work/os"
attach_ro "$sysos" "$work/sysos"
attach_ro "$appos" "$work/appos" raw
fi

mkdir -p "$(dirname "$out")"
[[ ${RESUME:-0} == 1 ]] || dd if=/dev/null of="$out" bs=1048576 seek="$mib" 2>/dev/null
dev=$(hdiutil attach -nomount -noverify -imagekey diskimage-class=CRawDiskImage "$out" | head -1 | awk '{print $1}')
devs+=("$dev")
[[ ${RESUME:-0} == 1 ]] || newfs_hfs -J -s -v iosroot "$dev"
mount -t hfs -o rw,owners "$dev" "$work/root"
root=$work/root

copy() {
	echo "copying $1 -> ${2#"$root"}"
	mkdir -p "$2"
	rsync -aH --numeric-ids "$1/" "$2/"
}
if [[ ${RESUME:-0} != 1 ]]; then
	copy "$work/os" "$root"
	copy "$work/sysos" "$root/private/preboot/Cryptexes/OS"
	copy "$work/appos" "$root/private/preboot/Cryptexes/App"
fi
df -h "$root"

# Shared cache where dyld looks at ignition level 1 (symlink, zero bytes).
mkdir -p "$root/System/Library/Caches"
rm -rf "$root/System/Library/Caches/com.apple.dyld"
ln -s ../../../private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld \
	"$root/System/Library/Caches/com.apple.dyld"

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file that has no counterpart in the OS root to its root path.
for c in OS App; do
	base=$root/private/preboot/Cryptexes/$c
	(cd "$base" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*' -print0) |
	while IFS= read -r -d '' f; do
		f=${f#./}
		if [[ ! -e $root/$f && ! -L $root/$f ]]; then
			mkdir -p "$root/$(dirname "$f")"
			if [[ -L $base/$f ]]; then cp -P "$base/$f" "$root/$f"; else ln "$base/$f" "$root/$f"; fi
		fi
	done
	echo "linked cryptex $c"
done

# The source mounts are noowners: everything comes out as root:admin (gid 80).
# iOS images that booted used root:wheel everywhere (NOTES v3: chown -R 0:0).
chown -R 0:0 "$root"

# logd's log stores (owner _logd 272 + .migrated marker).
db=$root/private/var/db
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated" 2>/dev/null || { : >"$db/$d/.migrated"; chown 272:272 "$db/$d/.migrated"; chmod 0644 "$db/$d/.migrated"; }
done

# System keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$root/private/var/keybags"
kb=$work/systembag.kb
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' >"$kb"
install -m 0600 -o 0 -g 0 "$kb" "$root/private/var/keybags/systembag.kb"
rm -f "$kb"

# Fake mobileactivationd system container so the data ark can be seeded.
cdir=$root/private/var/containers/Data/System/D90C94C4-F94E-4FCF-8A28-A93BE3846D8E
mkdir -p "$cdir"
python3 -I - "$cdir" <<'PY'
import plistlib, sys, os
d = sys.argv[1]
with open(os.path.join(d, '.com.apple.mobile_container_manager.metadata.plist'), 'wb') as f:
    plistlib.dump({'MCMMetadataIdentifier': 'com.apple.mobileactivationd',
                   'MCMMetadataUUID': 'D90C94C4-F94E-4FCF-8A28-A93BE3846D8E',
                   'MCMMetadataVersion': 1, 'MCMMetadataInfoFlags': 0}, f, fmt=plistlib.FMT_BINARY)
PY

python3 -I "$here/sweet-root-tweaks.py" "$root"

# lsd seeding deadlock: ICL __GenerateBuiltInAppInstallSessionID calls MG; make
# the bl at dsc.15 +0x1619F0C return 0 (NOTES v6: 02 25 5e 95 -> 00 00 80 d2).
python3 -I - "$root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.15" <<'PY'
import sys
p, off = sys.argv[1], 0x1619F0C
with open(p, 'r+b') as f:
    f.seek(off); cur = f.read(4)
    if cur == bytes.fromhex('02255e95'):
        f.seek(off); f.write(bytes.fromhex('000080d2')); print('dsc.15 patched')
    elif cur == bytes.fromhex('000080d2'):
        print('dsc.15 already patched')
    else:
        sys.exit('dsc.15 unexpected bytes %s at 0x%x' % (cur.hex(), off))
PY

# decmpfs: XNU has no decompressor, materialize every compressed file.
python3 -I "$here/strip-decmpfs.py" "$root"
tail -1 /tmp/strip-decmpfs.log

# ownership checks: system files root, mobile home 501.
chown -R 501:501 "$root/private/var/mobile"
chown 0:0 "$root/private/var/mobile/.."
rm -rf "$root"/{.fseventsd,.Spotlight-V100,.Trashes}
stat -f 'dsc owner %u:%g' "$root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.15"
stat -f 'mobile owner %u:%g' "$root/private/var/mobile"
df -h "$root"
umount "$root"
fsck_hfs -f -n "$dev"
echo "DONE: $out"
