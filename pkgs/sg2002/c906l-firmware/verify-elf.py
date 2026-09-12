#!/usr/bin/env python3
"""Check that every loadable firmware segment fits its exclusive carveout."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"C906L ELF validation failed: {message}")


def main() -> None:
    if len(sys.argv) != 5:
        fail("usage: verify-elf.py ELF START SIZE READELF")

    elf = Path(sys.argv[1])
    start = int(sys.argv[2], 0)
    size = int(sys.argv[3], 0)
    readelf = sys.argv[4]
    end = start + size

    output = subprocess.check_output(
        [readelf, "--wide", "--program-headers", str(elf)], text=True
    )
    entry_match = re.search(r"Entry point 0x([0-9a-fA-F]+)", output)
    if not entry_match:
        fail("readelf did not report an entry point")
    entry = int(entry_match.group(1), 16)
    # BLCP_2ND is packed as a flat binary and FSBL jumps to the configured
    # run address.  An entry merely somewhere inside the carveout would be
    # valid for ELF loading but silently wrong for this boot protocol.
    if entry != start:
        fail(f"entry {entry:#x} does not equal run address {start:#x}")

    load_count = 0
    for line in output.splitlines():
        fields = line.split()
        if not fields or fields[0] != "LOAD":
            continue
        if len(fields) < 7:
            fail(f"cannot parse LOAD line: {line!r}")
        address = int(fields[2], 16)
        file_size = int(fields[4], 16)
        memory_size = int(fields[5], 16)
        segment_end = address + max(file_size, memory_size)
        if address < start or segment_end > end:
            fail(
                f"LOAD [{address:#x}, {segment_end:#x}) is outside "
                f"[{start:#x}, {end:#x})"
            )
        load_count += 1

    if load_count == 0:
        fail("ELF has no loadable segments")


if __name__ == "__main__":
    main()
