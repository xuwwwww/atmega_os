#!/usr/bin/env python3

import argparse
import tempfile

from wsl_guest_upload import (
    BINARY_OP_COMMIT,
    BINARY_OP_DATA,
    BINARY_SYNC,
    binary_packet,
    crc16_ccitt,
    selected_guests,
)


def main():
    assert crc16_ccitt(b"123456789") == 0x29B1
    assert crc16_ccitt(b"") == 0xFFFF

    with tempfile.NamedTemporaryFile() as guest_file:
        guest_file.write(b"\x13\x00\x00\x00")
        guest_file.flush()
        args = argparse.Namespace(
            input=None,
            name=None,
            guest=[f"alpha={guest_file.name}"],
            entry=0,
        )
        selected = selected_guests(args)
        assert selected[0][0] == "alpha"
        assert selected[0][2] == b"\x13\x00\x00\x00"
        assert selected[0][4] == 0

    data_packet = binary_packet(BINARY_OP_DATA, 0x1234, b"abc")
    assert data_packet[:2] == BINARY_SYNC
    assert data_packet[2:6] == bytes([BINARY_OP_DATA, 0x34, 0x12, 3])
    assert data_packet[6:9] == b"abc"
    assert int.from_bytes(data_packet[-2:], "little") == crc16_ccitt(
        data_packet[2:-2]
    )
    commit_packet = binary_packet(BINARY_OP_COMMIT)
    assert commit_packet[2:6] == bytes([BINARY_OP_COMMIT, 0, 0, 0])
    print("guest upload CRC + binary packet tests passed")


if __name__ == "__main__":
    main()
