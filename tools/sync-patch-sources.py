#!/usr/bin/env python3
"""Keep the standalone source copies in xnu/patches/ in sync with the patches
that embed them, and fix the hunk line counts.

The CI applies xnu/patches/*.patch to a pristine tree.  A patch that adds a
new file (--- /dev/null) embeds the whole file, so editing the standalone
copy in xnu/patches/<name>.c is NOT enough: the embedded copy in the patch is
what the CI compiles.  This script rewrites the embedded copy from the
standalone one and recomputes every "@@ -0,0 +1,N @@" header.

Usage:  python3 tools/sync-patch-sources.py [--check]

Exit code 1 (with a report) if --check finds something out of sync.
"""
import os
import re
import sys

PATCH_DIR = "xnu/patches"

# file added by a patch  ->  the standalone copy that must match it
EMBEDDED = [
    ("0119-venus-haptics-cover.patch", "iokit/Kernel/arm/hid/SweetCoverSensor.cpp"),
    ("0088-venus-camera.patch", "iokit/Kernel/arm/SweetCamera.cpp"),
    ("0112-venus-gpu-probe.patch", "bsd/dev/sweet_gpu.c"),
    ("0113-venus-gpu-power.patch", "bsd/dev/sweet_gpu_power.c"),
    ("0114-venus-gpu-iokit-service.patch", "iokit/Kernel/arm/SweetGPU.cpp"),
    ("0115-venus-gic-smmu-probe.patch", "bsd/dev/sweet_gic.c"),
    ("0116-venus-key-events.patch", "bsd/dev/sweet_keyevents.c"),
    ("0119-venus-haptics-cover.patch", "iokit/Kernel/arm/hid/SweetHaptics.cpp"),
    ("0121-venus-perf-profile.patch", "bsd/dev/sweet_perf.c"),
    ("0122-venus-gpucc.patch", "bsd/dev/sweet_gpucc.c"),
    ("0123-venus-power-thermal.patch", "bsd/dev/sweet_powerinfo.c"),
    ("0124-venus-scm-helper.patch", "bsd/dev/sweet_scm.c"),
    ("0125-venus-panic-capture.patch", "bsd/dev/sweet_panic.c"),
]


def standalone_for(path):
    """The standalone copy name: bsd/dev/sweet_gpu.c -> sweet_gpu.c,
    iokit/Kernel/arm/SweetGPU.cpp -> SweetGPU.cpp"""
    return os.path.basename(path)


def read_standalone(path):
    with open(os.path.join(PATCH_DIR, standalone_for(path))) as f:
        return f.read()


def split_patch(text):
    """Yield (header, body) chunks of a unified diff."""
    parts = re.split(r"(?m)^(?=diff --git |--- )", text)
    return parts


def patch_for_file(patch_text, filepath):
    """Return (start, end, hunk_header_pos) of the block that adds `filepath`."""
    marker = "+++ b/" + filepath
    idx = patch_text.find(marker)
    if idx < 0:
        return None
    hunk = patch_text.find("@@ -0,0 +1,", idx)
    if hunk < 0:
        return None
    body_start = patch_text.find("\n", hunk) + 1
    # the block ends at the next file header
    nxt = re.search(r"(?m)^(diff --git |--- )", patch_text[body_start:])
    body_end = body_start + (nxt.start() if nxt else len(patch_text) - body_start)
    return hunk, body_start, body_end


def main():
    check = "--check" in sys.argv
    problems = []
    for patch_name, filepath in sorted(EMBEDDED):
        pp = os.path.join(PATCH_DIR, patch_name)
        if not os.path.exists(pp):
            continue
        text = open(pp).read()
        rng = patch_for_file(text, filepath)
        if rng is None:
            continue
        hunk, body_start, body_end = rng
        body = text[body_start:body_end]
        lines = [l[1:] for l in body.split("\n") if l.startswith("+")]
        src = read_standalone(filepath)
        want = src.split("\n")
        if want and want[-1] == "":
            want = want[:-1]
        if lines == want:
            # still verify the hunk count
            m = re.search(r"@@ -0,0 \+1,(\d+) @@", text[hunk:hunk + 60])
            if m and int(m.group(1)) != len(want):
                problems.append(f"{patch_name}: hunk count {m.group(1)} != {len(want)}")
                if not check:
                    text = (text[:hunk] + f"@@ -0,0 +1,{len(want)} @@" + text[hunk + m.end():])
            else:
                continue
        else:
            problems.append(f"{patch_name}: embedded copy of {filepath} differs from the standalone copy")
            if check:
                continue
            newbody = "\n".join("+" + l for l in want) + "\n"
            text = text[:body_start] + newbody + text[body_end:]
            hunk, body_start, body_end = patch_for_file(text, filepath)
            m = re.search(r"@@ -0,0 \+1,(\d+) @@", text[hunk:hunk + 60])
            if m:
                text = text[:hunk] + f"@@ -0,0 +1,{len(want)} @@" + text[hunk + m.end():]
        if not check:
            open(pp, "w").write(text)
            print(f"synced {patch_name}: {filepath} ({len(want)} lines)")

    if problems:
        print("\n".join(problems))
        if check:
            print("OUT OF SYNC")
            return 1
    else:
        print("all embedded copies are in sync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
