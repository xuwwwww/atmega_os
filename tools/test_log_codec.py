#!/usr/bin/env python3
"""Exercise raw escaping and split Huffman frames without serial hardware."""

import json
import sys
from pathlib import Path

from generate_log_codec import build_codes, encode
from wsl_serial_monitor import (
    FRAME_COMPRESSED,
    FRAME_ESCAPE,
    FRAME_PANIC,
    SerialDecoder,
)


def decode_one_byte_at_a_time(frame):
    decoder = SerialDecoder()
    output = bytearray()
    for byte in frame:
        output.extend(decoder.feed(bytes([byte])))
    return bytes(output)


def main():
    manifest_path = Path(sys.argv[1])
    messages = json.loads(manifest_path.read_text(encoding="utf-8"))
    codes = build_codes(messages)

    for name, message in messages.items():
        payload, bit_count = encode(message.encode("utf-8"), codes)
        frame = bytes(
            [
                FRAME_ESCAPE,
                FRAME_COMPRESSED,
                bit_count & 0xFF,
                bit_count >> 8,
                *payload,
            ]
        )
        actual = decode_one_byte_at_a_time(frame)
        expected = message.encode("utf-8")
        if actual != expected:
            raise AssertionError(f"{name}: {actual!r} != {expected!r}")

    raw_with_zero = b"A\x00B"
    escaped = b"A\x00\x00B"
    if decode_one_byte_at_a_time(escaped) != raw_with_zero:
        raise AssertionError("raw zero escaping failed")

    panic_frame = bytes([FRAME_ESCAPE, FRAME_PANIC, 2, 127, 1])
    panic_text = decode_one_byte_at_a_time(panic_frame)
    if b"KERNEL PANIC code=127 detail=1; rebooting" not in panic_text:
        raise AssertionError("panic frame decoding failed")

    print(
        f"codec tests passed: {len(messages)} messages + raw escaping "
        "+ panic framing"
    )


if __name__ == "__main__":
    main()
