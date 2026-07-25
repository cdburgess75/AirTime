# Stock firmware backup

**The 16 MB image is deliberately NOT stored in this repository.**

It contains the device's `settings` NVS partition, which holds **live WiFi
credentials** (`wifissid1` / `wifipass1` were non-empty on this unit). Publishing a
full flash image publishes those credentials. It is also AMNVOLT/ats-mini vendor
firmware, which is a separate reason not to redistribute it.

Keep the image locally and in private backup. Its checksum lives here so any copy
can be verified:

    shasum -a 256 -c SHA256SUMS      # macOS
    sha256sum -c SHA256SUMS          # Linux

Validate structure with:

    python3 tools/inspect_flash.py /path/to/stock-full-16mb.bin

## If you must share a flash image

Zero the `settings` partition (0x7e0000, length 0x10000) first — but understand the
result is **no longer a complete restore image**, because that partition is exactly
what the full backup existed to preserve (see MILESTONE0.md §2).
