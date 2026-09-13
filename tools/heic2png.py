#!/usr/bin/env python3
"""Convert HEIC/HEIF (iPhone photos) to PNG so they can be read as images.

    python3 tools/heic2png.py IMG_8011.heic [more.heic ...]
    python3 tools/heic2png.py /path/to/dir      # every .heic in the directory

Writes <name>.png beside each input and prints the output paths.
Downscales to 2000 px on the long edge; a 12 MP phone photo is far more
pixels than any screen-reading needs, and the smaller file loads faster.
"""
import sys, pathlib
from PIL import Image
import pillow_heif

pillow_heif.register_heif_opener()
MAXEDGE = 2000

def convert(src: pathlib.Path) -> pathlib.Path:
    im = Image.open(src)
    im = im.convert("RGB")
    if max(im.size) > MAXEDGE:
        im.thumbnail((MAXEDGE, MAXEDGE), Image.LANCZOS)
    dst = src.with_suffix(".png")
    im.save(dst, "PNG", optimize=True)
    return dst

def main(argv):
    if not argv:
        print(__doc__)
        return 1
    targets = []
    for a in argv:
        p = pathlib.Path(a)
        if p.is_dir():
            targets += sorted(q for q in p.iterdir()
                              if q.suffix.lower() in (".heic", ".heif"))
        else:
            targets.append(p)
    if not targets:
        print("no HEIC/HEIF files found", file=sys.stderr)
        return 1
    for src in targets:
        print(convert(src))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
