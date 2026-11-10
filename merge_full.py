"""Merge the esp-sdr image into a single 0x0 flash file."""
import os
import sys

BUILD = r"E:\esp32-c5 android\esp-sdr\build"
OUT = sys.argv[1] if len(sys.argv) > 1 else r"E:\esp32-c5 android\esp-sdr\firmware\esp32c5-lcd-full.bin"

segments = [
    (0x2000, os.path.join(BUILD, "bootloader", "bootloader.bin")),
    (0x8000, os.path.join(BUILD, "partition_table", "partition-table.bin")),
    (0x10000, os.path.join(BUILD, "esp_sdr.bin")),
]

blobs = []
for offset, path in segments:
    with open(path, "rb") as fh:
        data = fh.read()
    print("0x%06x  %-28s %d bytes" % (offset, os.path.basename(path), len(data)))
    blobs.append((offset, data))

end = max(off + len(data) for off, data in blobs)
buf = bytearray(b"\xff" * end)
for off, data in blobs:
    buf[off : off + len(data)] = data

with open(OUT, "wb") as fh:
    fh.write(buf)
print("wrote %s (%d bytes)" % (OUT, len(buf)))
