#!/usr/bin/env python3
"""Fail a build before static AVR data leaves too little runtime stack."""

import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf")
    parser.add_argument("--size-tool", default="avr-size")
    parser.add_argument("--reserve", type=int, default=256)
    args = parser.parse_args()

    output = subprocess.check_output(
        [args.size_tool, "-A", args.elf], text=True
    )
    sections = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] in (".data", ".bss", ".noinit"):
            sections[fields[0]] = int(fields[1], 0)
    used = sum(sections.values())
    available = 2048 - used
    print(f"SRAM guard: static={used}, runtime-reserve={available}, "
          f"required>={args.reserve}")
    if available < args.reserve:
        raise SystemExit(
            f"static SRAM leaves only {available} bytes; "
            f"at least {args.reserve} bytes are required for the runtime stack"
        )


if __name__ == "__main__":
    main()
