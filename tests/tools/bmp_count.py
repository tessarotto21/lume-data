#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Usage: bmp_count.py file.bmp RRGGBB: prints how many pixels of an uncompressed 24 / 32-bit BMP have that colour.
import struct
import sys

data = open(sys.argv[1], "rb").read()
want = bytes.fromhex(sys.argv[2])[::-1]  # BGR in the file
off = struct.unpack_from("<I", data, 10)[0]
w, h = struct.unpack_from("<ii", data, 18)
bpp = struct.unpack_from("<H", data, 28)[0]
if bpp not in (24, 32):
    sys.exit("unsupported BMP depth %d" % bpp)
step = bpp // 8
stride = (w * step + 3) & ~3
n = 0
for y in range(abs(h)):
    row = off + y * stride
    n += sum(1 for x in range(w) if data[row + x * step : row + x * step + 3] == want)
print(n)
