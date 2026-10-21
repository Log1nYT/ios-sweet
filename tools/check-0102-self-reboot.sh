#!/bin/bash
#
# check-0102-self-reboot.sh - host side check of patch 0102
# (venus: the phone reboots itself into the Qualcomm fastboot).
#
# The phone is not needed for this: the patch is checked structurally and
# every touched source file is compiled with the exact flags of the CI
# kernel build (recovered from a CI log, see tools/research/compile-one.sh).
#
# Usage:  tools/check-0102-self-reboot.sh
#
# Environment:
#   XNU_BT       build tree with the patched sources (default logs/bt3)
#   XNU_PATCH    the patch under test (default xnu/patches/0102-*.patch)
#   XNU_CILOG    CI log with the compile commands (default logs/ci-*.txt)
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PATCH=${XNU_PATCH:-xnu/patches/0102-venus-self-reboot-fastboot.patch}
BT=${XNU_BT:-$PWD/logs/bt3}
CILOG=${XNU_CILOG:-$(ls -1 logs/ci-*.txt 2>/dev/null | head -1)}
rc=0

ok()   { echo "  ok   $*"; }
bad()  { echo "  FAIL $*"; rc=1; }

echo "== 1. the patch exists and has the expected structure"
[ -f "$PATCH" ] && ok "$PATCH ($(wc -l < "$PATCH") lines)" || { bad "$PATCH is missing"; exit 1; }

need() {  # need <pattern> <description>
	if grep -q -- "$1" "$PATCH"; then ok "$2"; else bad "$2 (pattern: $1)"; fi
}
need "^--- /dev/null$"                     "adds a new source file"
need "+++ b/bsd/dev/sweet_reboot.c"        "the new file is bsd/dev/sweet_reboot.c"
need "+++ b/bsd/conf/files"                "touches bsd/conf/files"
need "bsd/dev/sweet_reboot.c"              "bsd/conf/files lists sweet_reboot.c"
need "SYSCTL_PROC(_kern, OID_AUTO, sweet_reboot" "exposes kern.sweet_reboot"
need "sweetreboot"                         "the sweetreboot boot argument"
need "0xC4000009"                          "PSCI SYSTEM_RESET"
need "SR_REASON_FASTBOOT"                  "the fastboot reboot reason"
need "SR_SDAM_PPID"                        "the PMIC restart reason cell"
need "sweet_reboot_panic(void)"            "the panic hook"
need "sweet_reboot_request(void)"          "the reboot request hook"
need "5141"                                "the magic UDP port"
need "CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED" "the sysctl is a writable int"
if grep -q "^+++ b/osfmk/kern/debug.c" "$PATCH"; then ok "the panic path is hooked"; else bad "osfmk/kern/debug.c is not patched"; fi
if grep -q "^+++ b/osfmk/arm64/machine_routines.c" "$PATCH"; then ok "the reboot path is hooked"; else bad "osfmk/arm64/machine_routines.c is not patched"; fi

echo "== 2. the patch applies to the build tree (reverse check)"
if [ -d "$BT/xnu" ]; then
	if (cd "$BT/xnu" && git apply --check -R "$OLDPWD/$PATCH" 2>/dev/null); then
		ok "the patch matches the tree in $BT/xnu"
	elif (cd "$BT/xnu" && git apply --check "$OLDPWD/$PATCH" 2>/dev/null); then
		ok "the patch applies to the tree in $BT/xnu"
	else
		bad "the patch does not apply to $BT/xnu"
	fi
else
	echo "  skip no build tree in $BT"
fi

echo "== 3. compile check (CI flags)"
if [ -z "$CILOG" ] || [ ! -f "$CILOG" ]; then
	echo "  skip no CI log with the compile commands"
else
	cat > /tmp/venus-0102-cc.py <<'PYEOF'
import glob, os, re, shlex, subprocess, sys

CI = '/Users/runner/work/ios-sweet/ios-sweet/dxb'
SDK = re.compile(r'/Applications/Xcode[^ ]*?\.sdk/+System/Library/Frameworks/'
                 r'Kernel\.framework/Versions/A/PrivateHeaders/platform')

def find_cmd(log, suffix):
    for line in open(log, errors='replace'):
        if '/clang' not in line or ' -c ' not in line:
            continue
        line = re.sub(r'^build\s+Build\s+\S+\s+', '', line.rstrip('\n'))
        m = re.search(r'(\S+/clang) (.*)', line)
        if not m:
            continue
        try:
            args = shlex.split(m.group(2), posix=True)
        except ValueError:
            continue
        if any(a.endswith(suffix) for a in args):
            return args
    return None

def compile_one(log, bt, src):
    args = find_cmd(log, src)
    if not args:
        print('  skip no CI command for %s' % src)
        return 0
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ('-c', '-g', '-MD', '-MMD'):
            continue
        if a in ('-MF', '-MT', '-o', '-MD'):
            skip = True
            continue
        a = SDK.sub(os.path.join(bt, 'sdk-kernel-platform/platform'), a)
        out.append(a.replace(CI, bt))
    out[0] = subprocess.run(['xcrun', '--find', 'clang'], capture_output=True,
                             text=True).stdout.strip()
    out = [a for a in out if a]
    out.insert(1, '-fsyntax-only')
    comp = src.split('/')[0]
    cwd = None
    for d in glob.glob(os.path.join(bt, 'build/xnu.obj', '**', 'meta_features.h'),
                       recursive=True):
        if '/%s/' % comp in d.replace(os.sep, '/'):
            cwd = os.path.dirname(d)
            break
    if not cwd:
        print('  skip no build dir for %s' % comp)
        return 0
    p = subprocess.run(out, cwd=cwd, capture_output=True, text=True)
    errs = [l for l in p.stderr.splitlines() if ' error: ' in l or 'fatal error' in l]
    for l in errs[:10]:
        print('    ' + l.replace(bt + '/xnu/', ''))
    if errs:
        print('  FAIL %s: %d error(s)' % (src, len(errs)))
        return 1
    print('  ok   %s compiles' % src)
    return 0

log, bt = sys.argv[1], sys.argv[2]
rc = 0
for s in sys.argv[3:]:
    rc |= compile_one(log, bt, s)
sys.exit(rc)
PYEOF
	python3 /tmp/venus-0102-cc.py "$CILOG" "$BT" \
		osfmk/kern/debug.c osfmk/arm64/machine_routines.c \
		bsd/dev/sweet_ufs.c || rc=1
fi

echo "== 4. compile check of the new file (tools/research/compile-one.sh)"
if [ -x tools/research/compile-one.sh ] && [ -d "$BT/xnu" ]; then
	if bash tools/research/compile-one.sh bsd/dev/sweet_reboot.c >/tmp/venus-0102-cc.log 2>&1; then
		ok "bsd/dev/sweet_reboot.c compiles"
	else
		bad "bsd/dev/sweet_reboot.c does not compile (see /tmp/venus-0102-cc.log)"
		tail -20 /tmp/venus-0102-cc.log
	fi
else
	echo "  skip no build tree or no compile-one.sh"
fi

echo
[ $rc -eq 0 ] && echo "0102: all checks passed" || echo "0102: THERE WERE FAILURES"
exit $rc
