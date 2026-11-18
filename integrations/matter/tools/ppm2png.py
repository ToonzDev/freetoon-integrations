#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG with only the standard library."""
import struct, sys, zlib

def convert(src, dst):
    with open(src, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        data = f.read()
    raw = b"".join(b"\x00" + data[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(dst, "wb") as f:
        f.write(png)

if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
