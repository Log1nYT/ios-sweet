"""Random-access reader for an Apple UDIF .dmg as a file-like object (for pyfsapfs)."""
import bz2, lzma, plistlib, struct, zlib
from bisect import bisect_right
from collections import OrderedDict
import lzfse


class UDIF:
    def __init__(self, path, cache_chunks=256):
        self.f = open(path, 'rb')
        self.f.seek(-512, 2)
        koly = self.f.read(512)
        assert koly[:4] == b'koly', 'not a UDIF image'
        xml_off, xml_len = struct.unpack('>QQ', koly[0xD8:0xE8])
        self.f.seek(xml_off)
        plist = plistlib.loads(self.f.read(xml_len))
        self.parts = []
        for ent in plist['resource-fork']['blkx']:
            blk = ent['Data']
            assert blk[:4] == b'mish'
            first, count, data_off = struct.unpack('>QQQ', blk[8:32])
            n = struct.unpack('>I', blk[200:204])[0]
            chunks = []
            for i in range(n):
                t, _c, s, sc, co, cl = struct.unpack('>IIQQQQ', blk[204 + 40 * i:244 + 40 * i])
                if t == 0xFFFFFFFF:
                    break
                chunks.append((first + s, sc, t, data_off + co, cl))
            self.parts.append((ent['Name'], first, count, chunks))
        self.cache = OrderedDict()
        self.cache_chunks = cache_chunks
        self.pos = 0
        self.chunks = sorted(c for p in self.parts for c in p[3])
        self.starts = [c[0] for c in self.chunks]
        last = self.chunks[-1]
        self.size = (last[0] + last[1]) * 512

    def _chunk(self, idx):
        if idx in self.cache:
            self.cache.move_to_end(idx)
            return self.cache[idx]
        sec, sc, t, off, ln = self.chunks[idx]
        out = sc * 512
        if t in (0, 2):
            data = b'\0' * out
        else:
            self.f.seek(off)
            raw = self.f.read(ln)
            if t == 1:
                data = raw
            elif t == 0x80000005:
                data = zlib.decompress(raw)
            elif t == 0x80000006:
                data = bz2.decompress(raw)
            elif t == 0x80000007:
                data = lzfse.decompress(raw)
            elif t == 0x80000008:
                data = lzma.decompress(raw)
            else:
                raise ValueError(hex(t))
        data = data[:out]
        self.cache[idx] = data
        if len(self.cache) > self.cache_chunks:
            self.cache.popitem(last=False)
        return data

    def get_size(self):
        return self.size

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def read(self, n=-1):
        if n < 0 or self.pos + n > self.size:
            n = self.size - self.pos
        out = bytearray()
        pos, end = self.pos, self.pos + n
        while pos < end:
            i = bisect_right(self.starts, pos // 512) - 1
            sec = self.chunks[i][0]
            d = self._chunk(i)
            base = sec * 512
            take = d[pos - base:min(len(d), end - base)]
            if not take:
                break
            out += take
            pos += len(take)
        self.pos = pos
        return bytes(out)


class Window:
    def __init__(self, base, off, size):
        self.base, self.off, self.size, self.pos = base, off, size, 0

    def get_size(self):
        return self.size

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def read(self, n=-1):
        if n < 0 or self.pos + n > self.size:
            n = self.size - self.pos
        self.base.seek(self.off + self.pos)
        d = self.base.read(n)
        self.pos += len(d)
        return d
