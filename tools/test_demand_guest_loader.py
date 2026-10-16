#!/usr/bin/env python3
"""Host-only checks for the interactive PC-backed guest loader."""

import io
import os
import tempfile

from wsl_serial_monitor import DemandGuestLoader, crc16_ccitt


class FakeProcess:
    def __init__(self):
        self.stdin = io.BytesIO()


def sent_since(process, position):
    value = process.stdin.getvalue()
    return value[position:], len(value)


def main():
    image = bytes(range(100))
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "demo.bin")
        with open(path, "wb") as guest_file:
            guest_file.write(image)

        process = FakeProcess()
        loader = DemandGuestLoader(process, [directory])
        position = 0

        loader.feed(b"HOST RUN demo 3\r\n")
        output, position = sent_since(process, position)
        crc = crc16_ccitt(image)
        assert output == f"guest bload demo 100 {crc:04x} 0\r".encode()
        assert loader.busy

        loader.feed(b"READY name=demo size=100 chunk=16 encoding=binary\r\n")
        packet, position = sent_since(process, position)
        assert packet[:2] == b"\xa5\x5a"
        assert packet[2] == 1 and packet[5] == 16

        for end in (16, 32, 48, 64, 80, 96, 100):
            loader.feed(
                f"BINARY ACK bytes={end} crc={crc16_ccitt(image[:end]):04x}\r\n".encode()
            )
            packet, position = sent_since(process, position)
            if end < 100:
                assert packet[:2] == b"\xa5\x5a" and packet[2] == 1
            else:
                assert packet[:2] == b"\xa5\x5a" and packet[2] == 2

        loader.feed(b"LOAD OK bytes=100\r\n")
        output, position = sent_since(process, position)
        assert output == b"run-cached demo 3\r"
        assert not loader.busy

    print("PC-backed demand loader tests passed")


if __name__ == "__main__":
    main()
