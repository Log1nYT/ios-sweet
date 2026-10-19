#!/usr/bin/env python3
"""Materialize HFS+ decmpfs-compressed files in place (image bring-up for venus).

XNU has no decmpfs decompressor registered (they live in closed-source kexts),
so every compressed file in an iOS root image fails to exec/read.  Detection is
by stat: a decmpfs file always stores less than its logical size, so
st_blocks*512 < st_size.  Writing the file back through the VFS makes macOS
materialize it (the kernel decompresses; no re-compression happens on plain
writes).  Inode, hard links, mode and times are preserved.

Usage: strip-decmpfs.py /Volumes/iosroot
"""
import os, shutil, stat, sys, tempfile, time

root = sys.argv[1] if len(sys.argv) > 1 else '/Volumes/iosroot'
t0 = time.time()
nfiles = nstrip = nerr = 0
written = 0
log = open('/tmp/strip-decmpfs.log', 'w', buffering=1)

for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [d for d in dirnames
                   if d not in ('.fseventsd', '.Spotlight-V100', '.Trashes')]
    for name in filenames:
        p = os.path.join(dirpath, name)
        nfiles += 1
        try:
            st = os.lstat(p)
            if not stat.S_ISREG(st.st_mode) or st.st_size == 0:
                continue
            if st.st_blocks * 512 >= st.st_size:
                continue
            mode = st.st_mode
            with tempfile.TemporaryFile() as t:
                with open(p, 'rb') as s:
                    shutil.copyfileobj(s, t, 1 << 20)
                t.seek(0)
                if not mode & 0o200:
                    os.chmod(p, mode | 0o200)
                with open(p, 'wb') as d:
                    shutil.copyfileobj(t, d, 1 << 20)
                os.chmod(p, mode)
            os.utime(p, (st.st_atime, st.st_mtime))
            nstrip += 1
            written += st.st_size
        except Exception as e:
            nerr += 1
            log.write('ERR %s: %s\n' % (p, e))
        if nfiles % 5000 == 0:
            log.write('%d scanned, %d materialized (%.2f GB), %d err, %.0fs\n' %
                      (nfiles, nstrip, written / 1e9, nerr, time.time() - t0))

log.write('DONE files=%d materialized=%d (%.2f GB) err=%d %.0fs\n' %
          (nfiles, nstrip, written / 1e9, nerr, time.time() - t0))
