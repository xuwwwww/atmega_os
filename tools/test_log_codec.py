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
    FRAME_SHELL_TEXT,
    FRAME_SYNC,
    SerialDecoder,
    TerminalInputFilter,
)


def decode_one_byte_at_a_time(frame):
    decoder = SerialDecoder()
    output = bytearray()
    for byte in frame:
        output.extend(decoder.feed(bytes([byte])))
    return bytes(output)


def escaped_frame(frame_type, body):
    physical = bytearray([FRAME_ESCAPE, FRAME_SYNC])
    for byte in bytes([frame_type]) + body:
        physical.append(byte)
        if byte == FRAME_ESCAPE:
            physical.append(FRAME_ESCAPE)
    return bytes(physical)


def main():
    manifest_path = Path(sys.argv[1])
    messages = json.loads(manifest_path.read_text(encoding="utf-8"))
    codes = build_codes(messages)

    for name, message in messages.items():
        payload, bit_count = encode(message.encode("utf-8"), codes)
        frame = escaped_frame(
            FRAME_COMPRESSED,
            bytes([bit_count & 0xFF, bit_count >> 8, *payload]),
        )
        actual = decode_one_byte_at_a_time(frame)
        expected = message.encode("utf-8")
        if actual != expected:
            raise AssertionError(f"{name}: {actual!r} != {expected!r}")

    raw_with_zero = b"A\x00B"
    escaped = b"A\x00\x00B"
    if decode_one_byte_at_a_time(escaped) != raw_with_zero:
        raise AssertionError("raw zero escaping failed")

    panic_frame = escaped_frame(FRAME_PANIC, bytes([2, 127, 1]))
    panic_text = decode_one_byte_at_a_time(panic_frame)
    if b"KERNEL PANIC code=127 detail=1; rebooting" not in panic_text:
        raise AssertionError("panic frame decoding failed")

    decoder = SerialDecoder()
    decoder.feed(escaped_frame(FRAME_COMPRESSED, bytes([32, 0, 0xAA]))[:-1])
    recovered = decoder.feed(panic_frame)
    if b"KERNEL PANIC code=127 detail=1; rebooting" not in recovered:
        raise AssertionError("frame resynchronization failed")

    terminal = TerminalInputFilter()
    if terminal.feed(b"help\r") != b"help\r":
        raise AssertionError("ordinary terminal input changed")
    terminal.feed(b"about\r")
    if terminal.feed(b"\x1b[A") != b"about":
        raise AssertionError("Up history recall failed")
    if terminal.feed(b"\x1b[A") != b"\b \b" * 5 + b"help":
        raise AssertionError("second Up history recall failed")
    if terminal.feed(b"\x1b[B") != b"\b \b" * 4 + b"about":
        raise AssertionError("Down history recall failed")

    history_decoder = SerialDecoder(terminal.render_history)
    history_frame = escaped_frame(
        FRAME_SHELL_TEXT, bytes([2, 0x1F, ord("H")])
    )
    if history_decoder.feed(history_frame) != b"1 help\r\n2 about\r\n":
        raise AssertionError("host-rendered history command failed")

    print(
        f"codec tests passed: {len(messages)} messages + raw escaping "
        "+ panic framing + shell history"
    )


if __name__ == "__main__":
    main()
