"""Binary property list (bplist00) reader with byte offsets.

The point of this module is not to replace ``plistlib`` (which cannot read
binary plists at all) but to be able to *edit* one: every object knows the
byte offset it lives at, so a value can be patched in place and the file
written back.

Supported objects: dict, array, set, ASCII string, UTF-16 string, int
(1/2/4/8/16 bytes), real (4/8 bytes), data (including the "count is in the
next object" variant 0x4F), bool, date, UID and null.

Example
-------
>>> import plistlib
>>> blob = plistlib.dumps({"ActivationState": "Activated"}, fmt=plistlib.FMT_BINARY)
>>> bp = Bplist(blob)
>>> bp.root
{'ActivationState': 'Activated'}
>>> bp.find(["ActivationState"]).offset          # where the string object starts
>>> bp.patch(["ActivationState"], b"Activated!")  # same length -> in place
"""

from __future__ import annotations

import datetime
import struct
from typing import Any, Dict, List, Optional, Sequence, Tuple, Union

__all__ = ["Bplist", "BplistError", "to_python", "format_tree"]

MAGIC = b"bplist00"


class BplistError(Exception):
    pass


# ---------------------------------------------------------------------------
# object wrappers
# ---------------------------------------------------------------------------


class Obj:
    """Base class of every parsed plist object."""

    kind = "object"
    __slots__ = ("index", "offset", "value_offset", "value_len")

    def __init__(self, index: int, offset: int, value_offset: int, value_len: int):
        self.index = index
        self.offset = offset              # byte offset of the object marker
        self.value_offset = value_offset  # byte offset of the payload
        self.value_len = value_len        # payload length in bytes

    @property
    def type_name(self) -> str:
        return self.kind

    def value(self) -> Any:
        raise NotImplementedError

    def __repr__(self) -> str:
        return "<%s @0x%x %r>" % (self.kind, self.offset, self.value())

    def __str__(self) -> str:
        return format_obj(self)


class ScalarObj(Obj):
    __slots__ = ("_value",)

    def __init__(self, index, offset, value_offset, value_len, value):
        Obj.__init__(self, index, offset, value_offset, value_len)
        self._value = value

    def value(self) -> Any:
        return self._value


class NullObj(ScalarObj):
    kind = "null"

    def __init__(self, index, offset):
        ScalarObj.__init__(self, index, offset, offset, 0, None)


class FillObj(ScalarObj):
    kind = "fill"

    def __init__(self, index, offset):
        ScalarObj.__init__(self, index, offset, offset, 0, None)


class BoolObj(ScalarObj):
    kind = "bool"

    def __init__(self, index, offset, value):
        ScalarObj.__init__(self, index, offset, offset, 0, value)


class IntObj(ScalarObj):
    kind = "int"

    def __init__(self, index, offset, value_offset, value_len, value):
        ScalarObj.__init__(self, index, offset, value_offset, value_len, value)


class RealObj(ScalarObj):
    kind = "real"

    def __init__(self, index, offset, value_offset, value_len, value):
        ScalarObj.__init__(self, index, offset, value_offset, value_len, value)


class DateObj(ScalarObj):
    kind = "date"

    def __init__(self, index, offset, value_offset, value_len, value):
        ScalarObj.__init__(self, index, offset, value_offset, value_len, value)


class UidObj(ScalarObj):
    kind = "uid"

    def __init__(self, index, offset, value_offset, value_len, value):
        ScalarObj.__init__(self, index, offset, value_offset, value_len, value)


class StringObj(ScalarObj):
    kind = "string"
    __slots__ = ("encoding",)

    def __init__(self, index, offset, value_offset, value_len, value, encoding="ascii"):
        ScalarObj.__init__(self, index, offset, value_offset, value_len, value)
        self.encoding = encoding

    def value(self) -> str:
        return self._value


class DataObj(ScalarObj):
    kind = "data"

    def __init__(self, index, offset, value_offset, value_len, value):
        ScalarObj.__init__(self, index, offset, value_offset, value_len, value)


class ArrayObj(Obj):
    kind = "array"
    __slots__ = ("items",)

    def __init__(self, index, offset, value_offset, value_len, items):
        Obj.__init__(self, index, offset, value_offset, value_len)
        self.items = items

    def value(self) -> List[Any]:
        return [i.value() for i in self.items]


class SetObj(ArrayObj):
    kind = "set"


class DictObj(Obj):
    kind = "dict"
    __slots__ = ("keys", "values")

    def __init__(self, index, offset, value_offset, value_len, keys, values):
        Obj.__init__(self, index, offset, value_offset, value_len)
        self.keys = keys
        self.values = values

    def value(self) -> Dict[Any, Any]:
        return {k.value(): v.value() for k, v in zip(self.keys, self.values)}

    def pairs(self):
        return list(zip(self.keys, self.values))

    def __getitem__(self, key):
        for k, v in self.pairs():
            if k.value() == key:
                return v
        raise KeyError(key)

    def __contains__(self, key) -> bool:
        return any(k.value() == key for k, _ in self.pairs())

    def __len__(self) -> int:
        return len(self.keys)

    def get(self, key, default=None):
        for k, v in self.pairs():
            if k.value() == key:
                return v
        return default


# ---------------------------------------------------------------------------
# the parser
# ---------------------------------------------------------------------------


class Bplist:
    """A parsed binary property list."""

    def __init__(self, data: bytes):
        self.data = bytes(data)
        if not self.data.startswith(MAGIC):
            raise BplistError("not a bplist00 file (magic %r)" % self.data[:8])
        trailer = self.data[-32:]
        (self.sort_version, self.offset_size, self.ref_size,
         self.num_objects, self.top_object, self.offset_table_offset) = \
            struct.unpack(">5xBBBQQQ", trailer)
        if self.offset_size < 1 or self.offset_size > 8:
            raise BplistError("bad offset size %d" % self.offset_size)
        if self.ref_size < 1 or self.ref_size > 8:
            raise BplistError("bad object ref size %d" % self.ref_size)
        self.offsets: List[int] = []
        pos = self.offset_table_offset
        for _ in range(self.num_objects):
            self.offsets.append(int.from_bytes(self.data[pos:pos + self.offset_size], "big"))
            pos += self.offset_size
        self._cache: Dict[int, Obj] = {}
        self.root = self.object(self.top_object)

    # -- object access ----------------------------------------------------
    def object(self, index: int) -> Obj:
        obj = self._cache.get(index)
        if obj is not None:
            return obj
        if index < 0 or index >= self.num_objects:
            raise BplistError("object index %d out of range" % index)
        off = self.offsets[index]
        obj = self._parse(index, off)
        self._cache[index] = obj
        return obj

    def object_at_offset(self, offset: int) -> Optional[Obj]:
        for i, off in enumerate(self.offsets):
            if off == offset:
                return self.object(i)
        return None

    def _parse(self, index: int, off: int) -> Obj:
        data = self.data
        if off >= len(data):
            raise BplistError("object %d at %#x is past the end" % (index, off))
        marker = data[off]
        hi, lo = marker >> 4, marker & 0xF

        if marker == 0x00:
            return NullObj(index, off)
        if marker == 0x08:
            return BoolObj(index, off, False)
        if marker == 0x09:
            return BoolObj(index, off, True)
        if marker == 0x0F:
            return FillObj(index, off)

        if hi == 0x1:                       # integer, 2**lo bytes
            n = 1 << lo
            raw = data[off + 1:off + 1 + n]
            if len(raw) != n:
                raise BplistError("truncated int at %#x" % off)
            value = int.from_bytes(raw, "big", signed=True)
            return IntObj(index, off, off + 1, n, value)

        if hi == 0x2:                       # real, 2**lo bytes
            n = 1 << lo
            raw = data[off + 1:off + 1 + n]
            if n == 4:
                value = struct.unpack(">f", raw)[0]
            elif n == 8:
                value = struct.unpack(">d", raw)[0]
            else:
                raise BplistError("bad real size %d at %#x" % (n, off))
            return RealObj(index, off, off + 1, n, value)

        if hi == 0x3 and lo == 0x3:          # date
            raw = data[off + 1:off + 9]
            seconds = struct.unpack(">d", raw)[0]
            # Apple epoch (2001-01-01)
            value = datetime.datetime(2001, 1, 1) + datetime.timedelta(seconds=seconds)
            return DateObj(index, off, off + 1, 8, value)

        if hi == 0x4:                       # data
            if lo == 0x0F:
                count, start = self._read_count(index, off)
            else:
                count, start = lo, off + 1
            raw = data[start:start + count]
            if len(raw) != count:
                raise BplistError("truncated data at %#x" % off)
            return DataObj(index, off, start, count, raw)

        if hi == 0x5:                       # ASCII string
            if lo == 0x0F:
                count, start = self._read_count(index, off)
            else:
                count, start = lo, off + 1
            raw = data[start:start + count]
            text = raw.decode("ascii", "replace")
            return StringObj(index, off, start, count, text, "ascii")

        if hi == 0x6:                       # UTF-16BE string
            if lo == 0x0F:
                count, start = self._read_count(index, off)
            else:
                count, start = lo, off + 1
            raw = data[start:start + count * 2]
            try:
                text = raw.decode("utf-16-be")
            except UnicodeDecodeError:
                text = raw.decode("utf-16-be", "surrogatepass")
            return StringObj(index, off, start, count * 2, text, "utf-16-be")

        if hi == 0x8:                       # UID
            n = lo + 1
            raw = data[off + 1:off + 1 + n]
            value = int.from_bytes(raw, "big")
            return UidObj(index, off, off + 1, n, value)

        if hi in (0xA, 0xC):                # array / set
            if lo == 0x0F:
                count, start = self._read_count(index, off)
            else:
                count, start = lo, off + 1
            items = []
            pos = start
            for _ in range(count):
                ref = self._read_ref(pos)
                pos += self.ref_size
                items.append(self.object(ref))
            cls = ArrayObj if hi == 0xA else SetObj
            return cls(index, off, start, pos - start, items)

        if hi == 0xD:                       # dict
            if lo == 0x0F:
                count, start = self._read_count(index, off)
            else:
                count, start = lo, off + 1
            keys, values = [], []
            pos = start
            for _ in range(count):
                keys.append(self.object(self._read_ref(pos)))
                pos += self.ref_size
            for _ in range(count):
                values.append(self.object(self._read_ref(pos)))
                pos += self.ref_size
            return DictObj(index, off, start, pos - start, keys, values)

        raise BplistError("unknown object marker %#x at %#x" % (marker, off))

    def _read_ref(self, pos: int) -> int:
        raw = self.data[pos:pos + self.ref_size]
        if len(raw) != self.ref_size:
            raise BplistError("truncated object reference at %#x" % pos)
        return int.from_bytes(raw, "big")

    def _read_count(self, index: int, off: int) -> Tuple[int, int]:
        """Decode the "0x?F" extended size.

        Returns ``(count, payload_offset)``.

        Two encodings exist in the wild:

        *inline* - the count is an integer object written right behind the
        marker (``5F 10 1B`` = an ASCII string of 27 characters).  This is
        what Apple (plutil) and python's plistlib write.

        *table* - the count is the value of the object with the index
        ``index + 1`` of the object table and the payload starts right behind
        the marker.  This is what the CFBinaryPlist documentation describes.
        """
        data = self.data
        if off + 1 < len(data):
            marker = data[off + 1]
            if 0x10 <= marker <= 0x13:          # an integer object
                n = 1 << (marker & 3)
                raw = data[off + 2:off + 2 + n]
                if len(raw) == n:
                    count = int.from_bytes(raw, "big", signed=(marker & 3) >= 3)
                    end = off + 2 + n
                    if 0 <= count and end + count <= len(data):
                        return count, end
        if index + 1 < self.num_objects:
            nxt = self.object(index + 1)
            if isinstance(nxt, IntObj):
                return nxt.value(), off + 1
        raise BplistError("cannot decode the extended size at %#x" % off)

    # -- lookups ----------------------------------------------------------
    def find(self, path: Sequence[Any]) -> Optional[Obj]:
        """Find an object by a key path, e.g. ``["ActivationState"]``."""
        if not path:
            return self.root
        node = self.root
        for key in path:
            if not isinstance(node, DictObj):
                return None
            nxt = None
            for k, v in node.pairs():
                if k.value() == key:
                    nxt = v
                    break
            if nxt is None:
                return None
            node = nxt
        return node

    def key_offsets(self) -> List[Tuple[str, int, int]]:
        """[(key, file offset of the key object, file offset of the value)]."""
        out = []

        def walk(node, prefix):
            if isinstance(node, DictObj):
                for k, v in node.pairs():
                    path = prefix + [k.value()]
                    out.append((".".join(str(p) for p in path), k.offset, v.offset))
                    walk(v, path)
            elif isinstance(node, ArrayObj):
                for i, v in enumerate(node.items):
                    walk(v, prefix + [i])

        walk(self.root, [])
        return out

    # -- editing ----------------------------------------------------------
    def patch(self, path: Sequence[Any], new_value: bytes) -> bytes:
        """Return a copy of the plist with the object at ``path`` replaced.

        ``new_value`` must have exactly the same length as the payload of the
        object, so that nothing else moves.
        """
        obj = self.find(path)
        if obj is None:
            raise KeyError("no such key path: %r" % (list(path),))
        if len(new_value) != obj.value_len:
            raise ValueError("value at %r is %d bytes, got %d"
                             % (list(path), obj.value_len, len(new_value)))
        buf = bytearray(self.data)
        buf[obj.value_offset:obj.value_offset + obj.value_len] = new_value
        return bytes(buf)

    def set_string(self, path: Sequence[Any], text: str) -> bytes:
        """Replace a string value, keeping the encoded length the same."""
        obj = self.find(path)
        if obj is None:
            raise KeyError("no such key path: %r" % (list(path),))
        enc = "ascii" if obj.encoding == "ascii" else "utf-16-be"
        raw = text.encode(enc)
        if len(raw) != obj.value_len:
            raise ValueError("value at %r is %d bytes, %r needs %d"
                             % (list(path), obj.value_len, text, len(raw)))
        return self.patch(path, raw)

    # -- printing ---------------------------------------------------------
    def tree(self, max_items: Optional[int] = None) -> str:
        out = ["plist: %d objects, root @0x%x" % (self.num_objects, self.root.offset)]
        out.append(format_tree(self.root, max_items=max_items))
        return "\n".join(out)

    def __str__(self) -> str:
        return self.tree()


# ---------------------------------------------------------------------------
# printing / conversion
# ---------------------------------------------------------------------------


def format_obj(obj: Obj) -> str:
    if isinstance(obj, StringObj):
        return "%s @0x%x %r" % (obj.kind, obj.offset, obj.value())
    if isinstance(obj, DataObj):
        return "%s @0x%x (%d bytes)" % (obj.kind, obj.offset, obj.value_len)
    if isinstance(obj, (ArrayObj, DictObj)):
        return "%s @0x%x (%d entries)" % (obj.kind, obj.offset, len(obj.items()))
    return "%s @0x%x %r" % (obj.kind, obj.offset, obj.value())


def format_tree(obj: Obj, indent: int = 0, max_items: Optional[int] = None) -> str:
    """Render an object tree with the byte offset of every object."""
    pad = "  " * indent
    lines = []
    if isinstance(obj, DictObj):
        lines.append("%s%s @0x%x (%d entries)" % (pad, obj.kind, obj.offset,
                                                  len(obj.pairs())))
        for i, (k, v) in enumerate(obj.pairs()):
            if max_items is not None and i >= max_items:
                lines.append("%s  ..." % pad)
                break
            lines.append("%s  key %s" % (pad, format_obj(k)))
            lines.append(format_tree(v, indent + 2, max_items))
    elif isinstance(obj, ArrayObj):
        lines.append("%s%s @0x%x (%d items)" % (pad, obj.kind, obj.offset,
                                                len(obj.items)))
        for i, v in enumerate(obj.items):
            if max_items is not None and i >= max_items:
                lines.append("%s  ..." % pad)
                break
            lines.append(format_tree(v, indent + 1, max_items))
    else:
        lines.append("%s%s" % (pad, format_obj(obj)))
    return "\n".join(lines)


def to_python(obj: Union[Obj, Any]) -> Any:
    """Convert a parsed object into plain python data."""
    if isinstance(obj, Obj):
        return obj.value()
    return obj
