# bmp2png.py <files or folders>: the PC build's shots (shots/NNNN.bmp) as PNG beside them, no libraries needed
import glob, os, struct, sys, zlib

def topng(src, dst):
    d = open(src, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bp = struct.unpack_from('<H', d, 28)[0] // 8
    row = (w * bp + 3) & ~3
    flip = h > 0
    h = abs(h)
    raw = bytearray()
    for y in range(h):
        sy = h - 1 - y if flip else y
        r = d[off + sy * row:off + sy * row + w * bp]
        out = bytearray(w * 3)
        out[0::3] = r[2::bp]; out[1::3] = r[1::bp]; out[2::3] = r[0::bp]
        raw += b'\0' + out
    ch = lambda t, x: struct.pack('>I', len(x)) + t + x + struct.pack('>I', zlib.crc32(t + x) & 0xffffffff)
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                          ch(b'IDAT', zlib.compress(bytes(raw), 6)) + ch(b'IEND', b''))

for a in sys.argv[1:]:
    for f in sorted(glob.glob(os.path.join(a, '*.bmp')) if os.path.isdir(a) else [a]):
        topng(f, f[:-4] + '.png')
