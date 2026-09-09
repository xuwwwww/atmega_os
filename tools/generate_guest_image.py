#!/usr/bin/env python3
"""Convert a raw guest binary into an AVR PROGMEM C array."""

import argparse
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--c-source", required=True)
    parser.add_argument("--c-header", required=True)
    parser.add_argument("--capacity", type=int, required=True)
    return parser.parse_args()


def main():
    args = parse_args()
    payload = Path(args.input).read_bytes()
    if not payload:
        raise SystemExit("guest image is empty")
    if len(payload) > args.capacity:
        raise SystemExit(
            f"guest image is {len(payload)} bytes; capacity is {args.capacity}"
        )

    rows = []
    for offset in range(0, len(payload), 8):
        values = ", ".join(f"0x{value:02x}u" for value in payload[offset:offset + 8])
        rows.append(f"    {values},")

    header = f"""#ifndef RV_GUEST_IMAGE_H
#define RV_GUEST_IMAGE_H

#include <stdint.h>

#define RV_GUEST_IMAGE_SIZE {len(payload)}u

extern const uint8_t rv_guest_image[RV_GUEST_IMAGE_SIZE];

#endif
"""
    source = """#include \"rv_guest_image.h\"

#include <avr/pgmspace.h>

const uint8_t rv_guest_image[RV_GUEST_IMAGE_SIZE] PROGMEM =
{
""" + "\n".join(rows) + "\n};\n"

    Path(args.c_header).write_text(header, encoding="utf-8")
    Path(args.c_source).write_text(source, encoding="utf-8")


if __name__ == "__main__":
    main()
