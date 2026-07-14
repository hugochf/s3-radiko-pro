#!/usr/bin/env python3
"""Pretty-print the device's persistent event log (Phase 20).

Usage:
    parttool.py -p PORT read_partition --partition-name=elog --output elog.bin
    python3 tools/elog_dump.py elog.bin

The partition is a raw text ring: written lines, then an erased (0xFF) gap at
the write head. Bytes AFTER the gap are the oldest (pre-wrap), bytes before it
the newest, so we print gap-to-end first, then start-to-gap.
"""
import sys

def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()

    gap_start = data.find(b"\xff")
    if gap_start < 0:
        ordered = data                      # ring completely full
    else:
        gap_end = gap_start
        while gap_end < len(data) and data[gap_end] == 0xFF:
            gap_end += 1
        ordered = data[gap_end:] + data[:gap_start]

    text = ordered.decode("utf-8", "replace")
    count = 0
    for line in text.splitlines():
        if line.strip():
            print(line)
            count += 1
    print(f"--- {count} lines ---", file=sys.stderr)

if __name__ == "__main__":
    main()
