#!/bin/bash
#
# relink.sh - rebuild the SWEET kernel LOCALLY from a CI artifact, replacing a few objects.
#
#   tools/research/relink.sh ARTIFACT_DIR OUT_KERNEL [bsd/dev/x.c=xnu/patches/x.c ...]
#
# ARTIFACT_DIR is a downloaded `xnu-kernel` artifact (`gh run download RUN -n xnu-kernel -D DIR`); it must
# hold kernel.development.sweet.link/{kernel.development.sweet.a,.linkarguments,.alias.exp} and
# libfirehose_kernel.a (uploaded by the workflow since 2026-10-23).  Each `REL=SRC` argument compiles
# the source SRC as the xnu file REL (with the exact CI flags, tools/research/compile-one.sh), swaps the
# object into a COPY of the archive and relinks.  Nothing in ARTIFACT_DIR is modified.
#
# Caveat (measured 2026-10-23): Xcode 27's ld lays the segments out differently from the CI kernel (no
# __LASTDATA_CONST/__CTF/__LINKINFO, symbols shifted).  Boot-test an UNCHANGED relink before trusting it.
set -euo pipefail

ART=${1:?artifact dir}
OUT=${2:?output kernel path}
shift 2

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
L="$ART/kernel.development.sweet.link"
FH="$ART/libfirehose_kernel.a"
CLANG=${CLANG:-/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang}
BT=${BT:-$ROOT/logs/bt3}
WORK=$(mktemp -d /tmp/relink.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

[ -f "$L/kernel.development.sweet.a" ] || { echo "no $L/kernel.development.sweet.a"; exit 1; }
[ -f "$FH" ] || { echo "no $FH (the artifact is older than the workflow change)"; exit 1; }

cp "$L/kernel.development.sweet.a" "$WORK/k.a"
mkdir -p "$WORK/obj"

for spec in "$@"; do
	rel=${spec%%=*}
	src=${spec#*=}
	[ "$rel" != "$spec" ] || src="$ROOT/$rel"
	case "$src" in /*) ;; *) src="$ROOT/$src" ;; esac
	dst="$BT/xnu/$rel"
	bak=
	if [ -f "$dst" ]; then bak="$WORK/bak.$(echo "$rel" | tr / _)"; cp "$dst" "$bak"; fi
	mkdir -p "$(dirname "$dst")"
	cp "$src" "$dst"
	OUTDIR="$WORK/obj" "$ROOT/tools/research/compile-one.sh" -q -o "$rel" || { [ -n "$bak" ] && cp "$bak" "$dst"; exit 1; }
	if [ -n "$bak" ]; then cp "$bak" "$dst"; else rm -f "$dst"; fi
	member=$(basename "${rel%.*}").o
	cp "$WORK/obj/$(echo "$rel" | tr / _).o" "$WORK/$member"
	ar -t "$WORK/k.a" | grep -qx "$member" || echo "note: $member is a NEW archive member"
	(cd "$WORK" && ar rc k.a "$member")
	echo "replaced $member"
done

args=$(cat "$L/kernel.development.sweet.linkarguments")
args=${args//-L\/Users\/runner\/work\/ios-sweet\/ios-sweet\/dxb\/fakeroot\/usr\/local\/lib\/kernel/-L$ART}
# shellcheck disable=SC2086
"$CLANG" $args -Wl,-read_only_relocs,suppress -Wl,-alias_list,"$L/kernel.development.sweet.alias.exp" \
	-Wl,-force_load,"$WORK/k.a" -o "$OUT" 2>&1 | grep -v -E "segment_order|obsolete|built for newer" || true
[ -f "$OUT" ] || { echo "link failed"; exit 1; }
ls -l "$OUT" | awk '{print "relinked", $5, "bytes ->", $9}'
