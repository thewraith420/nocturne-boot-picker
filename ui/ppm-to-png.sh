#!/bin/sh
# Converts the PPMs that PICKER_SCREENSHOT_DIR produces into PNGs.
#
#   ./ppm-to-png.sh /tmp/shots/*.ppm
#
# picker writes PPM because it is ~15 lines of code and needs no zlib
# inside the initramfs. Converting happens here instead, on a normal
# machine. Uses ImageMagick when available and otherwise falls back to
# python3's standard library - no pillow, no extra packages, so this
# works on the Slate itself.
set -eu

[ $# -gt 0 ] || { echo "usage: ppm-to-png.sh <file.ppm> [...]" >&2; exit 1; }

if command -v magick >/dev/null 2>&1; then conv() { magick "$1" "$2"; }
elif command -v convert >/dev/null 2>&1; then conv() { convert "$1" "$2"; }
elif command -v python3 >/dev/null 2>&1; then
    conv() { python3 - "$1" "$2" <<'PY'
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
# P6 header: magic, width, height, maxval - any of which may be
# separated by arbitrary whitespace or comment lines.
fields, pos = [], 2
while len(fields) < 3:
    while pos < len(data) and data[pos:pos+1].isspace(): pos += 1
    if data[pos:pos+1] == b"#":
        while data[pos:pos+1] not in (b"\n", b""): pos += 1
        continue
    start = pos
    while pos < len(data) and not data[pos:pos+1].isspace(): pos += 1
    fields.append(int(data[start:pos]))
pos += 1
w, h, _maxv = fields
px = data[pos:pos + w * h * 3]
raw = b"".join(b"\x00" + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(tag, body):
    return (struct.pack(">I", len(body)) + tag + body
            + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 9))
       + chunk(b"IEND", b""))
with open(dst, "wb") as f:
    f.write(png)
PY
    }
else
    echo "ppm-to-png: need ImageMagick or python3" >&2
    exit 1
fi

for f in "$@"; do
    out=${f%.ppm}.png
    conv "$f" "$out"
    echo "  $f -> $out"
done
