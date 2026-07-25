#!/usr/bin/env python3
"""Inspect (and compare) ESP32 flash images — AirTime Milestone 0 helper.

    python3 tools/inspect_flash.py IMAGE.bin
        Validate an image and show what each partition actually contains.

    python3 tools/inspect_flash.py BEFORE.bin AFTER.bin
        Compare two images partition by partition. Use this after a restore
        (MILESTONE0.md §4d) to prove the device really came back.

The partition table is decoded from the image itself, so this works on any
ESP32 image, not just one particular layout.

Deliberately dependency-free and Python 3.9 compatible: it has to run on a
stock macOS with nothing but Apple's Command Line Tools installed.
"""

import hashlib
import struct
import sys

PART_TABLE_OFFSET = 0x8000
PART_TABLE_SIZE = 0xC00
ENTRY_MAGIC = 0x50AA
MD5_MAGIC = 0xEBEB
IMAGE_MAGIC = 0xE9

TYPES = {0: "app", 1: "data"}
DATA_SUBTYPES = {
    0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump",
    0x04: "nvs_keys", 0x05: "efuse", 0x06: "undefined",
    0x81: "fat", 0x82: "spiffs", 0x83: "littlefs",
}

# After a restore the device boots and writes to some partitions before you can
# read it back. Those differences are expected; the rest are not.
MAY_DIFFER = {"nvs", "otadata", "coredump", "phy_init"}


def read_image(path):
    with open(path, "rb") as f:
        return f.read()


def parse_partitions(data):
    """Decode the partition table. Returns [(name, offset, size, type, subtype)]."""
    out = []
    table = data[PART_TABLE_OFFSET:PART_TABLE_OFFSET + PART_TABLE_SIZE]
    for i in range(0, len(table), 32):
        entry = table[i:i + 32]
        if len(entry) < 32:
            break
        magic, ptype, subtype, offset, size = struct.unpack("<HBBII", entry[:12])
        if magic == MD5_MAGIC:
            continue
        if magic != ENTRY_MAGIC:
            break
        label = entry[12:28].rstrip(b"\x00").decode("utf-8", "replace")
        out.append((label, offset, size, ptype, subtype))
    return out


def used_fraction(region):
    if not region:
        return 0.0
    return 1.0 - region.count(b"\xff") / len(region)


def describe(ptype, subtype):
    tn = TYPES.get(ptype, "0x%02x" % ptype)
    if ptype == 0:
        sn = "factory" if subtype == 0 else "ota_%d" % (subtype - 0x10)
    else:
        sn = DATA_SUBTYPES.get(subtype, "0x%02x" % subtype)
    return "%s/%s" % (tn, sn)


def inspect(path):
    data = read_image(path)
    print("image        : %s" % path)
    print("size         : %d bytes (%.2f MB)" % (len(data), len(data) / 1048576.0))
    print("sha256       : %s" % hashlib.sha256(data).hexdigest())

    ok = True
    if len(data) < PART_TABLE_OFFSET + PART_TABLE_SIZE:
        print("\n!! image is too small to contain a partition table")
        return False

    boot_ok = data[0] == IMAGE_MAGIC
    tbl_ok = data[PART_TABLE_OFFSET] == 0xAA and data[PART_TABLE_OFFSET + 1] == 0x50
    print("bootloader   : 0x%02x %s" % (data[0], "OK" if boot_ok else "BAD (expect 0xe9)"))
    print("part. table  : 0x%02x 0x%02x %s" % (
        data[PART_TABLE_OFFSET], data[PART_TABLE_OFFSET + 1],
        "OK" if tbl_ok else "BAD (expect 0xaa 0x50)"))
    ok = ok and boot_ok and tbl_ok

    parts = parse_partitions(data)
    if not parts:
        print("\n!! no partitions decoded")
        return False

    print("\n%-12s %-14s %10s %10s %8s  %s" %
          ("label", "type", "offset", "size", "used", "head"))
    highest = 0
    for label, offset, size, ptype, subtype in parts:
        region = data[offset:offset + size]
        highest = max(highest, offset + size)
        head = region[:8].hex() if region else ""
        flag = ""
        if ptype == 0:  # an app partition that holds data must look like an image
            if used_fraction(region) > 0 and region[:1] != bytes([IMAGE_MAGIC]):
                flag = "  << no image header!"
                ok = False
        print("%-12s %-14s 0x%08x 0x%08x %7.1f%%  %s%s" % (
            label, describe(ptype, subtype), offset, size,
            used_fraction(region) * 100, head, flag))

    print("\nhighest partition end : 0x%x (%.2f MB)" % (highest, highest / 1048576.0))
    if len(data) < highest:
        print("!! IMAGE IS TRUNCATED — it ends at 0x%x, before the last partition"
              % len(data))
        ok = False
    else:
        tail = data[highest:]
        if tail:
            print("unpartitioned tail    : %.1f%% used" % (used_fraction(tail) * 100))

    print("\n%s" % ("image looks valid" if ok else "!! PROBLEMS FOUND — see above"))
    return ok


def compare(before_path, after_path):
    before = read_image(before_path)
    after = read_image(after_path)
    print("before : %s (%d bytes)" % (before_path, len(before)))
    print("after  : %s (%d bytes)" % (after_path, len(after)))

    if hashlib.sha256(before).hexdigest() == hashlib.sha256(after).hexdigest():
        print("\nidentical — byte-for-byte match.")
        return True

    parts = parse_partitions(before)
    if not parts:
        print("\n!! cannot decode a partition table from the 'before' image")
        return False

    print("\n%-12s %-10s %s" % ("label", "status", "note"))
    unexpected = []
    for label, offset, size, _ptype, _subtype in parts:
        b = before[offset:offset + size]
        a = after[offset:offset + size]
        if b == a:
            print("%-12s %-10s" % (label, "same"))
        elif label in MAY_DIFFER:
            print("%-12s %-10s written by the device on boot — expected" % (label, "differs"))
        else:
            print("%-12s %-10s UNEXPECTED" % (label, "differs"))
            unexpected.append(label)

    # The bootloader region ahead of the partition table matters too.
    if before[:PART_TABLE_OFFSET] != after[:PART_TABLE_OFFSET]:
        print("%-12s %-10s UNEXPECTED" % ("<bootloader>", "differs"))
        unexpected.append("<bootloader>")

    if unexpected:
        print("\n!! %d partition(s) differ unexpectedly: %s" %
              (len(unexpected), ", ".join(unexpected)))
        return False
    print("\nall differences are in partitions the device rewrites on boot — restore OK.")
    return True


def main(argv):
    if len(argv) == 2:
        return 0 if inspect(argv[1]) else 1
    if len(argv) == 3:
        return 0 if compare(argv[1], argv[2]) else 1
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
