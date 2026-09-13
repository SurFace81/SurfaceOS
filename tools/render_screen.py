#!/usr/bin/env python3
"""Render a QEMU screendump (PPM) into a coarse ASCII picture (8x16 cells)."""
import sys


def main(path):
    data = open(path, 'rb').read()
    if data[:2] != b'P6':
        print('not a P6 ppm')
        return 1
    pos = 2
    vals = []
    while len(vals) < 3:
        while data[pos] in b' \t\n':
            pos += 1
        start = pos
        while data[pos] not in b' \t\n':
            pos += 1
        vals.append(int(data[start:pos]))
    pos += 1
    w, h, mv = vals
    pix = data[pos:pos + w * h * 3]

    for by in range(h // 16):
        line = ''
        for bx in range(w // 8):
            lit = False
            for yy in range(0, 16, 2):
                for xx in range(0, 8, 2):
                    i = ((by * 16 + yy) * w + bx * 8 + xx) * 3
                    if pix[i] > 40 or pix[i + 1] > 40 or pix[i + 2] > 40:
                        lit = True
                        break
                if lit:
                    break
            line += '#' if lit else '.'
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/screen.ppm'))
