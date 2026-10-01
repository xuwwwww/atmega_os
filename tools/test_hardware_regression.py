#!/usr/bin/env python3

from hardware_regression import assess_output


def main():
    images = {
        "bitwalk": ("bitwalk.bin", 151, 0x2891),
        "checksum": ("checksum.bin", 155, 0xD8CA),
        "fibonacci": ("fibonacci.bin", 155, 0xEA5E),
    }
    valid = """
rv32 checksum exit=8
verify checksum ptr=1 size=192 crc=d8ca/d8ca
rv32 bitwalk exit=39
verify bitwalk ptr=1 size=192 crc=2891/2891
rv32 fibonacci exit=64
verify fibonacci ptr=1 size=192 crc=ea5e/ea5e
"""
    passed, exits, faults, reasons = assess_output(0, valid, images)
    assert passed
    assert exits == {"checksum": 8, "bitwalk": 39, "fibonacci": 64}
    assert not faults
    assert not reasons

    broken = valid.replace("rv32 checksum exit=8", "rv32 checksum fault=4 pc=104")
    passed, _exits, faults, reasons = assess_output(1, broken, images)
    assert not passed
    assert faults["checksum"] == (4, 104)
    assert "runner exited 1" in reasons
    assert "checksum fault=4 pc=104" in reasons
    print("hardware regression parser tests passed")


if __name__ == "__main__":
    main()
