"""Draw the app icon (a gun sight, no game art): quest/tools/make_icon.py
writes quest/vc3/src/main/res/drawable-*/ic_launcher.png."""
import math, os, struct, zlib

def png(path, n):
    rows = []
    c = (n - 1) / 2.0
    for y in range(n):
        row = bytearray(b'\0')
        for x in range(n):
            d = math.hypot(x - c, y - c) / (n / 2.0)
            r, g, b, a = 24, 26, 34, 255                       # dark disc
            if d > 0.96:
                a = 0
            elif abs(d - 0.62) < 0.06 or d < 0.08:             # ring, centre dot
                r, g, b = 230, 40, 30
            elif (abs(x - c) < n * 0.025 or abs(y - c) < n * 0.025) and 0.2 < d < 0.85:
                r, g, b = 240, 240, 240                        # cross hairs
            row += bytes((r, g, b, a))
        rows.append(bytes(row))
    raw = zlib.compress(b''.join(rows), 9)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0))
    data += chunk(b'IDAT', raw) + chunk(b'IEND', b'')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(data)

res = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vc3', 'src', 'main', 'res')
for name, n in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    png(os.path.join(res, 'drawable-' + name, 'ic_launcher.png'), n)
