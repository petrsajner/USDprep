# A .tx texture without maketx: what maketx writes is a tiled TIFF whose
# further IFDs are the mip levels. This writes exactly that (uncompressed,
# 8-bit RGB, one solid colour) - enough to ask Nuke whether it reads the
# format at all.   python make_tx.py out.tx R G B [size]
import struct, sys


def ifd_bytes(width, height, tile, data_offset, data_len, next_ifd_pos_holder, description):
    entries = []

    def add(tag, typ, count, value):
        entries.append((tag, typ, count, value))

    add(254, 4, 1, 0 if description else 1)   # NewSubfileType: 1 = reduced-resolution (a mip level)
    add(256, 4, 1, width)
    add(257, 4, 1, height)
    add(258, 3, 3, None)                      # BitsPerSample -> offset, filled below
    add(259, 3, 1, 1)                         # no compression
    add(262, 3, 1, 2)                         # RGB
    add(277, 3, 1, 3)                         # SamplesPerPixel
    add(284, 3, 1, 1)                         # contiguous
    add(322, 4, 1, tile)                      # TileWidth
    add(323, 4, 1, tile)                      # TileLength
    add(324, 4, 1, data_offset)               # TileOffsets
    add(325, 4, 1, data_len)                  # TileByteCounts
    return sorted(entries)


def main():
    out, r, g, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    size = int(sys.argv[5]) if len(sys.argv) > 5 else 64
    tile = 64
    levels = []
    s = size
    while True:
        levels.append(s)
        if s == 1:
            break
        s = max(1, s // 2)

    blob = bytearray(b"II*\x00\x00\x00\x00\x00")  # header, first IFD offset patched later
    prev_next_field = 4
    for level, dim in enumerate(levels):
        # a tile is always tile x tile samples, padded
        pixels = bytes([r, g, b]) * (tile * tile)
        data_offset = len(blob)
        blob += pixels
        bits_offset = len(blob)
        blob += struct.pack("<HHH", 8, 8, 8)
        entries = ifd_bytes(dim, dim, tile, data_offset, len(pixels), None, level == 0)
        if len(blob) % 2:
            blob += b"\x00"
        ifd_offset = len(blob)
        struct.pack_into("<I", blob, prev_next_field, ifd_offset)
        blob += struct.pack("<H", len(entries))
        for tag, typ, count, value in entries:
            if tag == 258:
                blob += struct.pack("<HHII", tag, typ, count, bits_offset)
            elif typ == 3:
                blob += struct.pack("<HHIHH", tag, typ, count, value, 0)
            else:
                blob += struct.pack("<HHII", tag, typ, count, value)
        prev_next_field = len(blob)
        blob += struct.pack("<I", 0)
    open(out, "wb").write(blob)


main()
