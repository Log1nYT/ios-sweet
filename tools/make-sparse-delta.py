#!/usr/bin/env python3
"""Build an Android sparse *delta* image between two raw disk images.

Usage: make-sparse-delta.py OLD NEW OUT.sparse [--block 4096]

Compares OLD and NEW in whole blocks (file holes count as zero) and emits a
sparse image containing ONLY the changed blocks as RAW chunks; everything
else becomes DONT_CARE chunks (skipped by the bootloader, so the existing
partition content is left untouched).  Flashing such a delta takes seconds
instead of minutes.

The tool self-verifies the emitted file: chunk tiling must cover the whole
image contiguously, and every RAW chunk's payload must equal NEW's bytes at
that offset.
"""

import argparse
import os
import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_FILL = 0xCAC2
CHUNK_DONT_CARE = 0xCAC3

WIN = 1 << 20  # compare window


def data_extents(fd):
    """Return sorted list of (start, end) byte ranges physically allocated."""
    size = os.fstat(fd).st_size
    exts = []
    pos = 0
    while pos < size:
        try:
            d = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError:
            break
        if d >= size:
            break
        try:
            h = os.lseek(fd, d, os.SEEK_HOLE)
        except OSError:
            h = size
        if h <= d:
            break
        exts.append((d, min(h, size)))
        pos = h + 1
    return exts


def read_span(fd, exts, off, size, fill=0):
    """Read [off, off+size) filling holes with bytes([fill])."""
    buf = bytearray([fill]) * size
    end = off + size
    # binary search for first extent that could overlap
    lo, hi = 0, len(exts)
    while lo < hi:
        mid = (lo + hi) // 2
        if exts[mid][1] <= off:
            lo = mid + 1
        else:
            hi = mid
    i = lo
    while i < len(exts) and exts[i][0] < end:
        s, e = exts[i]
        s2, e2 = max(s, off), min(e, end)
        if s2 < e2:
            os.lseek(fd, s2, os.SEEK_SET)
            chunk = os.read(fd, e2 - s2)
            buf[s2 - off:s2 - off + len(chunk)] = chunk
        i += 1
    return bytes(buf)


def merge_extents(a, b):
    """Merge two sorted extent lists into non-overlapping sorted ranges."""
    pts = sorted(a + b)
    out = []
    for s, e in pts:
        if out and s <= out[-1][1]:
            if e > out[-1][1]:
                out[-1][1] = e
        else:
            out.append([s, e])
    return [(s, e) for s, e in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("out")
    ap.add_argument("--block", type=int, default=4096)
    args = ap.parse_args()

    blk = args.block
    fo = os.open(args.old, os.O_RDONLY)
    fn = os.open(args.new, os.O_RDONLY)
    size_o = os.fstat(fo).st_size
    size_n = os.fstat(fn).st_size
    if size_o != size_n:
        sys.exit(f"size mismatch: {size_o} != {size_n}")
    if size_n % blk:
        sys.exit(f"size {size_n} not a multiple of {blk}")
    total_blocks = size_n // blk

    exts_o = data_extents(fo)
    exts_n = data_extents(fn)
    print(f"old: {size_o} bytes, {len(exts_o)} extents")
    print(f"new: {size_n} bytes, {len(exts_n)} extents")
    print(f"logical blocks: {total_blocks}")

    # --- find changed blocks -------------------------------------------------
    changed = []  # sorted list of changed block indices
    merged = merge_extents(exts_o, exts_n)
    done = 0
    for s, e in merged:
        for w in range(s, e, WIN):
            we = min(w + WIN, e)
            bo = read_span(fo, exts_o, w, we - w)
            bn = read_span(fn, exts_n, w, we - w)
            if bo == bn:
                continue
            for i in range(0, we - w, blk):
                if bo[i:i + blk] != bn[i:i + blk]:
                    changed.append((w + i) // blk)
        done = e
        if done % (1 << 26) < WIN:  # progress every ~64 MiB
            print(f"  compared up to {done >> 20} MiB", flush=True)
    changed.sort()
    print(f"changed blocks: {len(changed)} ({(len(changed) * blk) >> 10} KiB)")

    # --- group changed blocks into contiguous ranges -------------------------
    ranges = []
    for b in changed:
        if ranges and b == ranges[-1][1]:
            ranges[-1][1] = b + 1
        else:
            ranges.append([b, b + 1])

    # --- build chunk list ----------------------------------------------------
    chunks = []  # (type, start_block, nblocks, data)
    pos = 0
    for rs, re in ranges:
        if rs > pos:
            chunks.append([CHUNK_DONT_CARE, pos, rs - pos, None])
        data = read_span(fn, exts_n, rs * blk, (re - rs) * blk)
        # split the range into all-zero sub-runs (FILL) and data (RAW)
        i = 0
        n = re - rs
        while i < n:
            if data[i * blk:(i + 1) * blk] == b"\x00" * blk:
                j = i + 1
                while j < n and data[j * blk:(j + 1) * blk] == b"\x00" * blk:
                    j += 1
                chunks.append([CHUNK_FILL, rs + i, j - i, b"\x00\x00\x00\x00"])
                i = j
            else:
                j = i + 1
                while j < n and data[j * blk:(j + 1) * blk] != b"\x00" * blk:
                    j += 1
                chunks.append([CHUNK_RAW, rs + i, j - i, data[i * blk:j * blk]])
                i = j
        pos = re
    if pos < total_blocks:
        chunks.append([CHUNK_DONT_CARE, pos, total_blocks - pos, None])

    raw_b = sum(c[2] for c in chunks if c[0] == CHUNK_RAW)
    fill_b = sum(c[2] for c in chunks if c[0] == CHUNK_FILL)
    dc_b = sum(c[2] for c in chunks if c[0] == CHUNK_DONT_CARE)
    assert raw_b + fill_b + dc_b == total_blocks, "tiling error"
    print(f"chunks: {len(chunks)} | raw {raw_b} blk ({raw_b * blk >> 20} MiB) | "
          f"fill {fill_b} blk | dont-care {dc_b} blk")

    # --- write sparse file ---------------------------------------------------
    with open(args.out, "wb") as f:
        hdr = struct.pack("<IHHHHIIII", SPARSE_MAGIC, 1, 0, 28, 12, blk,
                          total_blocks, len(chunks), 0)
        f.write(hdr)
        for ctype, _, nblk, data in chunks:
            if ctype == CHUNK_RAW:
                total_sz = 12 + nblk * blk
            elif ctype == CHUNK_FILL:
                total_sz = 16
            else:
                total_sz = 12
            f.write(struct.pack("<HHII", ctype, 0, nblk, total_sz))
            if data is not None:
                f.write(data)
    out_sz = os.path.getsize(args.out)
    print(f"wrote {args.out}: {out_sz} bytes ({out_sz >> 20} MiB)")

    # --- self-verify ---------------------------------------------------------
    print("verifying...", flush=True)
    with open(args.out, "rb") as f:
        magic, major, minor, fhs, chs, bs, tb, tc, _ = struct.unpack("<IHHHHIIII",
                                                                     f.read(28))
        assert magic == SPARSE_MAGIC and bs == blk and tb == total_blocks
        assert tc == len(chunks), f"chunk count {tc} != {len(chunks)}"
        cur = 0
        for ci in range(tc):
            ctype, _, nblk, total_sz = struct.unpack("<HHII", f.read(12))
            if ctype == CHUNK_RAW:
                data = f.read(nblk * blk)
                assert len(data) == nblk * blk
                want = read_span(fn, exts_n, cur * blk, nblk * blk)
                if data != want:
                    sys.exit(f"VERIFY FAIL: raw chunk {ci} mismatch at block {cur}")
            elif ctype == CHUNK_FILL:
                pat = f.read(4)
                s, e = cur * blk, (cur + nblk) * blk
                # check all-zero in new over this span (only allocated parts can differ)
                for w in range(s, e, WIN):
                    we = min(w + WIN, e)
                    bn = read_span(fn, exts_n, w, we - w)
                    if bn != b"\x00" * (we - w):
                        sys.exit(f"VERIFY FAIL: fill chunk {ci} non-zero in new at {w}")
            elif ctype == CHUNK_DONT_CARE:
                pass
            else:
                sys.exit(f"VERIFY FAIL: unexpected chunk type {ctype:#x}")
            cur += nblk
        assert cur == total_blocks, f"tiling {cur} != {total_blocks}"
    print("verify OK")
    os.close(fo)
    os.close(fn)


if __name__ == "__main__":
    main()
