#!/usr/bin/env python3
"""Convert HEIC/HEIF photos to PNG so they can actually be looked at.

    tools/heic2png.py IMG_8006.HEIC            -> IMG_8006.png
    tools/heic2png.py ~/Desktop/*.HEIC         -> alongside each source
    tools/heic2png.py IMG.HEIC -o /tmp/shot.png

iPhones shoot HEIC by default and most tooling cannot read it. Screen photos
of the radio are the main way this project gets evidence from hardware, so a
photo that cannot be opened is a debugging session that does not happen.

Downscales to 2000 px on the long edge by default: a 12 MP screen photo is
mostly empty desk, and the panel is 320x170.
"""
import sys, os
from PIL import Image
import pillow_heif

pillow_heif.register_heif_opener()


def convert(src, dst=None, max_edge=2000):
    if dst is None:
        dst = os.path.splitext(src)[0] + ".png"
    im = Image.open(src)
    im = im.convert("RGB")
    if max_edge and max(im.size) > max_edge:
        scale = max_edge / max(im.size)
        im = im.resize((round(im.width * scale), round(im.height * scale)),
                       Image.LANCZOS)
    im.save(dst, "PNG")
    return dst, im.size


def main():
    args = [a for a in sys.argv[1:] if a != "-o"]
    out = None
    if "-o" in sys.argv:
        out = sys.argv[sys.argv.index("-o") + 1]
        args = [a for a in args if a != out]
    if not args:
        sys.exit(__doc__)
    for src in args:
        try:
            dst, size = convert(src, out if len(args) == 1 else None)
            print(f"{src} -> {dst}  {size[0]}x{size[1]}")
        except Exception as e:                       # noqa: BLE001
            print(f"{src}: FAILED — {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
