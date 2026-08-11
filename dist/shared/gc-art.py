#!/usr/bin/env python3
"""Extract the game's own artwork from the user's own disc and saves.

    gc-art.py <package-root>

Writes, when the sources are present:

    art/banner.png     96x32  from game/files/opening.bnr -- the disc banner
    art/icon.png       32x32  from a .gci save -- the memory-card icon
    art/icon-<n>.png   32x32  the remaining animation frames
    art/title.txt             the disc's own title/maker/description strings

NOTHING HERE SHIPS. This artwork belongs to the game's publisher, so it is
extracted on the player's machine from the disc image they supplied and the
saves they made -- the same line setup.sh already draws for the recompiled
module. Do not add these outputs to a package.

Standard library only: setup.sh requires python3 and nothing else, so zlib
writes the PNGs and struct reads the formats. No PIL, no ImageMagick.

THE TRAP IN BOTH FORMATS: GameCube textures are stored in TILES, not in raster
order -- 4x4 pixels for RGB5A3, 8x4 for CI8. Reading linearly produces a
correctly-coloured but scrambled image, which looks like a plausible bug
anywhere else in the pipeline.
"""
import os
import struct
import sys
import zlib


def rgb5a3(v):
    """0x8000 set: opaque RGB555. Clear: ARGB3444."""
    if v & 0x8000:
        r, g, b = (v >> 10) & 0x1F, (v >> 5) & 0x1F, v & 0x1F
        return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)
    a = (v >> 12) & 0x7
    r, g, b = (v >> 8) & 0xF, (v >> 4) & 0xF, v & 0xF
    return (r * 17, g * 17, b * 17, a * 255 // 7)


def untile_rgb5a3(data, off, w, h):
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    i = off
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    px[y][x] = rgb5a3(struct.unpack_from('>H', data, i)[0])
                    i += 2
    return px, i


def untile_ci8(data, off, w, h, pal):
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    i = off
    for ty in range(0, h, 4):
        for tx in range(0, w, 8):      # CI8 tiles are 8x4, not 4x4
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 8):
                    px[y][x] = pal[data[i]]
                    i += 1
    return px, i


def write_png(path, px):
    h, w = len(px), len(px[0])
    raw = b''.join(b'\x00' + b''.join(bytes(p) for p in row) for row in px)

    def chunk(tag, body):
        c = tag + body
        return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c))

    with open(path, 'wb') as fh:
        fh.write(b'\x89PNG\r\n\x1a\n'
                 + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                 + chunk(b'IDAT', zlib.compress(raw, 9))
                 + chunk(b'IEND', b''))


def extract_banner(bnr_path, art):
    """opening.bnr: 'BNR1' + 28 pad, 96x32 RGB5A3, then 320 bytes of strings."""
    data = open(bnr_path, 'rb').read()
    if data[:4] not in (b'BNR1', b'BNR2') or len(data) < 0x1820:
        return False
    write_png(os.path.join(art, 'banner.png'), untile_rgb5a3(data, 0x20, 96, 32)[0])

    meta = data[0x1820:]
    lines = []
    for label, off, size in (('title', 0, 32), ('maker', 32, 32),
                             ('full_title', 64, 64), ('developer', 128, 64),
                             ('description', 192, 128)):
        s = meta[off:off + size].split(b'\x00')[0].decode('latin-1').strip()
        if s:
            lines.append(f'{label}={s}')
    if lines:
        with open(os.path.join(art, 'title.txt'), 'w') as fh:
            fh.write('\n'.join(lines) + '\n')
    return True


def extract_icon(gci_path, art):
    """A .gci is a 64-byte DEntry then the save blocks; the DEntry says where
    the banner and icon sit inside them and how each is encoded."""
    d = open(gci_path, 'rb').read()
    if len(d) < 0x40:
        return 0
    bannerfmt = d[0x07] & 3
    iconaddr = struct.unpack_from('>I', d, 0x2C)[0]
    iconfmt = struct.unpack_from('>H', d, 0x30)[0]

    pos = 0x40 + iconaddr
    if bannerfmt == 2:                       # RGB5A3 banner
        pos += 96 * 32 * 2
    elif bannerfmt == 1:                     # CI8 banner, palette follows it
        pos += 96 * 32 + 512

    frames = []
    for n in range(8):
        f = (iconfmt >> (2 * n)) & 3
        if f == 0:
            break
        frames.append((f, pos))
        pos += 32 * 32 * (2 if f == 2 else 1)

    if not frames:
        return 0
    shared = None
    if any(f == 1 for f, _ in frames):       # shared CI8 palette trails the frames
        if pos + 512 > len(d):
            return 0
        shared = [rgb5a3(struct.unpack_from('>H', d, pos + 2 * i)[0]) for i in range(256)]

    written = 0
    for n, (f, off) in enumerate(frames):
        if off + 32 * 32 * (2 if f == 2 else 1) > len(d):
            break
        px = (untile_rgb5a3(d, off, 32, 32)[0] if f == 2
              else untile_ci8(d, off, 32, 32, shared)[0])
        write_png(os.path.join(art, 'icon.png' if n == 0 else f'icon-{n}.png'), px)
        written += 1
    return written


def find_save(root):
    """First .gci under userdata/. Prefer the real card over NetPlayTemp, whose
    copy is scratch state rather than the player's own save."""
    hits = []
    for base, _, files in os.walk(os.path.join(root, 'userdata')):
        for f in files:
            if f.lower().endswith('.gci'):
                hits.append(os.path.join(base, f))
    hits.sort(key=lambda p: ('NetPlayTemp' in p, p))
    return hits[0] if hits else None


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    art = os.path.join(root, 'art')
    os.makedirs(art, exist_ok=True)

    bnr = os.path.join(root, 'game', 'files', 'opening.bnr')
    if os.path.isfile(bnr) and extract_banner(bnr, art):
        print('    banner: art/banner.png')
    else:
        print('    banner: no opening.bnr found')

    gci = find_save(root)
    if gci:
        n = extract_icon(gci, art)
        if n:
            print(f'    icon:   art/icon.png ({n} frame(s), from {os.path.basename(gci)})')
        else:
            print('    icon:   save found but no icon in it')
    else:
        print('    icon:   no save yet -- rerun after playing once')


if __name__ == '__main__':
    main()
