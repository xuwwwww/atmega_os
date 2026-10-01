#!/usr/bin/env python3
"""Upload one or more named RV32I guest images through one serial session."""

import argparse
import os
import select
import subprocess
import sys
import time

from wsl_serial_monitor import SerialDecoder

BINARY_GUEST_CHUNK_SIZE = 48
BINARY_SYNC = b"\xa5\x5a"
BINARY_OP_DATA = 1
BINARY_OP_COMMIT = 2

def crc16_ccitt(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def parse_args():
    parser = argparse.ArgumentParser(
        description="Load an RV32I .bin into ATmega OS SRAM without reflashing"
    )
    parser.add_argument("--script", required=True)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--input")
    parser.add_argument(
        "--entry", type=lambda value: int(value, 0), default=0,
        help="guest entry-point byte offset (default: 0)",
    )
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--run-all", action="store_true")
    parser.add_argument(
        "--run-name",
        metavar="NAME",
        help="run one named guest after loading, without closing the COM session",
    )
    parser.add_argument(
        "--prefix",
        action="store_true",
        help="enable named guest output prefixes before a requested run",
    )
    parser.add_argument(
        "--trace",
        action="store_true",
        help="enable sampled guest switch messages before a requested run",
    )
    parser.add_argument("--slot", type=int, choices=range(8), default=0)
    parser.add_argument("--name")
    parser.add_argument(
        "--guest",
        action="append",
        default=[],
        metavar="NAME=FILE",
        help="named image to upload; repeat to keep one COM connection open",
    )
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument(
        "--run-timeout",
        type=float,
        default=120.0,
        help="seconds to wait for a requested guest run; upload ACKs still use --timeout",
    )
    parser.add_argument("--retries", type=int, default=5)
    parser.add_argument("--fail-on-reboot", action="store_true")
    parser.add_argument(
        "--reset-on-open",
        action="store_true",
        help="request one deliberate DTR reset before rebuilding an SRAM batch",
    )
    return parser.parse_args()


class ShellConnection:
    def __init__(self, process):
        self.process = process
        self.decoder = SerialDecoder()
        self.decoded = bytearray()

    def send(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def wait_for(self, marker, timeout):
        marker = marker.encode("ascii")
        deadline = time.monotonic() + timeout

        while marker not in self.decoded:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                shown = self.decoded.decode("utf-8", errors="replace")
                raise TimeoutError(f"timed out waiting for {marker!r}; received:\n{shown}")
            readable, _, _ = select.select(
                [self.process.stdout.fileno()], [], [], remaining
            )
            if not readable:
                continue
            raw = os.read(self.process.stdout.fileno(), 4096)
            if not raw:
                raise RuntimeError(
                    f"serial relay exited with code {self.process.poll()}"
                )
            decoded = self.decoder.feed(raw)
            if decoded:
                os.write(sys.stdout.fileno(), decoded)
                self.decoded.extend(decoded)

        marker_end = self.decoded.index(marker) + len(marker)
        del self.decoded[:marker_end]

    def wait_for_any(self, markers, timeout):
        encoded = [marker.encode("ascii") for marker in markers]
        deadline = time.monotonic() + timeout

        while True:
            matches = [
                (self.decoded.find(marker), marker)
                for marker in encoded
                if self.decoded.find(marker) >= 0
            ]
            if matches:
                position, marker = min(matches, key=lambda match: match[0])
                del self.decoded[: position + len(marker)]
                return marker.decode("ascii")

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                shown = self.decoded.decode("utf-8", errors="replace")
                raise TimeoutError(
                    f"timed out waiting for one of {markers!r}; received:\n{shown}"
                )
            readable, _, _ = select.select(
                [self.process.stdout.fileno()], [], [], remaining
            )
            if not readable:
                continue
            raw = os.read(self.process.stdout.fileno(), 4096)
            if not raw:
                raise RuntimeError(
                    f"serial relay exited with code {self.process.poll()}"
                )
            decoded = self.decoder.feed(raw)
            if decoded:
                os.write(sys.stdout.fileno(), decoded)
                self.decoded.extend(decoded)

    def wait_for_prompt(self, timeout):
        """Accept Unix-style role prompts plus pre-2026 legacy prompts."""
        return self.wait_for_any(
            ["uno$ ", "nano$ ", "os$ ", "uno> ", "nano> ", "os> "], timeout
        )

    def wait_for_line(self, prefix, timeout):
        prefix = prefix.encode("ascii")
        deadline = time.monotonic() + timeout

        while True:
            line_end = self.decoded.find(b"\n")
            while line_end >= 0:
                line = bytes(self.decoded[: line_end + 1]).strip()
                del self.decoded[: line_end + 1]
                if line.startswith(prefix):
                    return line.decode("ascii")
                line_end = self.decoded.find(b"\n")

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                shown = self.decoded.decode("utf-8", errors="replace")
                raise TimeoutError(
                    f"timed out waiting for line {prefix!r}; received:\n{shown}"
                )
            readable, _, _ = select.select(
                [self.process.stdout.fileno()], [], [], remaining
            )
            if not readable:
                continue
            raw = os.read(self.process.stdout.fileno(), 4096)
            if not raw:
                raise RuntimeError(
                    f"serial relay exited with code {self.process.poll()}"
                )
            decoded = self.decoder.feed(raw)
            if decoded:
                os.write(sys.stdout.fileno(), decoded)
                self.decoded.extend(decoded)


class UploadAttemptError(Exception):
    pass


class DeviceRebootError(UploadAttemptError):
    pass


def expect_upload_result(connection, success, timeout):
    failures = [
        "LOAD ERROR", "BINARY NACK", "BINARY TIMEOUT",
        "unknown command", "ATmega OS",
    ]
    result = connection.wait_for_any([success, *failures], timeout)
    if result == "ATmega OS":
        raise DeviceRebootError("device rebooted")
    if result != success:
        raise UploadAttemptError(f"device returned {result!r}")


def binary_packet(operation, offset=0, payload=b""):
    if len(payload) > BINARY_GUEST_CHUNK_SIZE:
        raise ValueError("binary guest payload is too large")
    body = bytes(
        [operation, offset & 0xFF, (offset >> 8) & 0xFF, len(payload)]
    ) + payload
    crc = crc16_ccitt(body)
    return BINARY_SYNC + body + bytes([crc & 0xFF, crc >> 8])


def upload_image_binary(connection, image, crc, timeout, name, entry, retries):
    connection.decoded.clear()
    connection.send(
        f"\rguest bload {name} {len(image)} {crc:04x} {entry}\r".encode(
            "ascii"
        )
    )
    expect_upload_result(
        connection,
        f"READY name={name} size={len(image)} chunk=48 encoding=binary",
        timeout,
    )

    for offset in range(0, len(image), BINARY_GUEST_CHUNK_SIZE):
        chunk = image[offset : offset + BINARY_GUEST_CHUNK_SIZE]
        end = offset + len(chunk)
        stored_crc = crc16_ccitt(image[:end])
        expected = f"BINARY ACK bytes={end} crc={stored_crc:04x}"
        for ack_attempt in range(1, retries + 1):
            connection.decoded.clear()
            connection.send(binary_packet(BINARY_OP_DATA, offset, chunk))
            try:
                expect_upload_result(connection, expected, timeout)
                break
            except (TimeoutError, UploadAttemptError) as error:
                if isinstance(error, DeviceRebootError) or ack_attempt == retries:
                    raise
                print(
                    f"\nbinary ACK for offset {offset} failed ({error}); "
                    "resending the absolute-offset packet"
                )

    connection.decoded.clear()
    connection.send(binary_packet(BINARY_OP_COMMIT))
    expect_upload_result(connection, "LOAD OK", timeout)
    connection.wait_for_prompt(timeout)


def validate_guest(name, path, entry=0):
    with open(path, "rb") as guest_file:
        image = guest_file.read()
    if not 1 <= len(image) <= 192:
        raise SystemExit(
            f"guest image {path!r} must be 1..192 bytes, got {len(image)}"
        )
    if not name or len(name) >= 12 or any(
        not (character.islower() or character.isdigit() or character in "_-")
        for character in name
    ):
        raise SystemExit(
            "guest name must be 1..11 lowercase letters, digits, '_' or '-'"
        )
    if entry < 0 or entry >= len(image) or entry & 3:
        raise SystemExit(
            f"guest entry {entry} must be aligned to 4 and inside "
            f"the {len(image)}-byte image"
        )
    return name, path, image, crc16_ccitt(image), entry


def selected_guests(args):
    selected = []
    if args.input:
        name = args.name or os.path.splitext(os.path.basename(args.input))[0]
        selected.append(validate_guest(name, args.input, args.entry))
    for specification in args.guest:
        if "=" not in specification:
            raise SystemExit("--guest must be NAME=FILE")
        name, path = specification.split("=", 1)
        selected.append(validate_guest(name, path))
    if not selected:
        raise SystemExit("provide --input FILE or at least one --guest NAME=FILE")
    if len({name for name, _, _, _, _ in selected}) != len(selected):
        raise SystemExit("guest names must be unique within one upload batch")
    return selected


def synchronize_shell(connection, timeout):
    # Opening an Uno-style COM port can pulse DTR even when the relay requests
    # DTR-low.  Allow the bootloader to leave, then ask for a fresh prompt.
    time.sleep(2.0)
    connection.decoded.clear()
    connection.send(b"\r")
    connection.wait_for_prompt(timeout)


def set_shell_option(connection, command, timeout):
    connection.decoded.clear()
    connection.send((command + "\r").encode("ascii"))
    connection.wait_for("ok", timeout)
    connection.wait_for_prompt(timeout)


def wait_for_named_guest_results(connection, names, timeout):
    """Wait for every asynchronous VM job, not merely the shell prompt."""
    remaining = set(names)
    deadline = time.monotonic() + timeout

    while remaining:
        markers = ["ATmega OS"]
        for name in sorted(remaining):
            markers.append(f"rv32 {name} exit=")
            markers.append(f"rv32 {name} fault=")

        result = connection.wait_for_any(
            markers, max(0.0, deadline - time.monotonic())
        )
        if result == "ATmega OS":
            raise DeviceRebootError("device rebooted while guests were running")
        if " fault=" in result:
            name = result[len("rv32 ") : result.index(" fault=")]
            raise UploadAttemptError(f"guest {name!r} faulted")

        name = result[len("rv32 ") : result.index(" exit=")]
        remaining.remove(name)

    # Do not close the relay at the instant the final EXIT line arrives.  The
    # firmware emits its integrity record and shell prompt immediately after
    # that line; waiting for the prompt makes automated hardware validation
    # deterministic and preserves the last diagnostic.
    connection.wait_for_prompt(max(0.0, deadline - time.monotonic()))


def report_reboot_diagnostics(connection, timeout):
    """Read the fresh shell's reset/UART evidence before SRAM is rebuilt."""
    try:
        connection.wait_for_prompt(timeout)
        connection.decoded.clear()
        connection.send(b"faults\r")
        report = connection.wait_for_line("reset_cause=", timeout)
        print(f"\nunexpected reboot diagnostics: {report}")
        connection.wait_for_prompt(timeout)
    except (TimeoutError, RuntimeError, BrokenPipeError) as error:
        print(f"\nunexpected reboot diagnostics unavailable: {error}")


def main():
    args = parse_args()
    guests = selected_guests(args)
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        args.script,
        "-PortName",
        args.port,
        "-BaudRate",
        str(args.baud),
    ]
    if args.reset_on_open:
        command.append("-ResetOnOpen")
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )

    try:
        connection = ShellConnection(process)
        saw_reboot = False
        if args.retries < 1:
            raise ValueError("--retries must be at least 1")
        synchronize_shell(connection, args.timeout)
        uploaded = []
        attempt = 1
        while attempt <= args.retries:
            try:
                uploaded.clear()
                for name, _path, image, crc, entry in guests:
                    upload_image_binary(
                        connection, image, crc, args.timeout, name, entry,
                        args.retries,
                    )
                    uploaded.append((name, len(image), crc, entry))
                break
            except DeviceRebootError as error:
                saw_reboot = True
                if attempt == args.retries:
                    raise
                report_reboot_diagnostics(connection, args.timeout)
                print(
                    f"\nbatch attempt {attempt} failed: {error}; "
                    "the reboot erased SRAM, restarting from the first guest"
                )
                time.sleep(2.0)
                connection.decoded.clear()
                connection.send(b"\r")
                connection.wait_for_prompt(args.timeout)
                attempt += 1
            except (TimeoutError, UploadAttemptError) as error:
                if attempt == args.retries:
                    raise
                print(
                    f"\nbatch attempt {attempt} failed: {error}; "
                    "restarting from the first guest"
                )
                time.sleep(0.25)
                connection.decoded.clear()
                connection.send(b"\r")
                connection.wait_for_prompt(args.timeout)
                attempt += 1

        if saw_reboot and args.fail_on_reboot:
            print("\nupload eventually succeeded after a device reboot; "
                  "batch must restore every earlier guest")
            return 2

        if args.prefix:
            set_shell_option(connection, "guest prefix on", args.timeout)
        if args.trace:
            set_shell_option(connection, "guest trace on", args.timeout)

        if args.run or args.run_all or args.run_name:
            if args.run_name:
                run_command = f"guest {args.run_name} -run\r".encode("ascii")
            else:
                run_command = b"guest runall\r" if args.run_all else b"guest run\r"
            connection.send(run_command)
            if args.run_all:
                wait_for_named_guest_results(
                    connection,
                    [name for name, _size, _crc, _entry in uploaded],
                    args.run_timeout,
                )
            elif args.run_name:
                wait_for_named_guest_results(
                    connection, [args.run_name], args.run_timeout
                )
            else:
                result = connection.wait_for_any(
                    ["rv32 C guest exit=", "ATmega OS"],
                    args.run_timeout,
                )
                if result == "ATmega OS":
                    raise DeviceRebootError(
                        "device rebooted while guest was running"
                    )
        for name, size, crc, entry in uploaded:
            print(
                f"\nloaded {name}: {size} bytes, CRC16-CCITT={crc:04x}, "
                f"entry={entry}"
            )
        return 0
    except DeviceRebootError as error:
        print(f"\nupload failed: {error}", file=sys.stderr)
        report_reboot_diagnostics(connection, args.timeout)
        return 2
    except (TimeoutError, RuntimeError, BrokenPipeError,
            UploadAttemptError) as error:
        print(f"\nupload failed: {error}", file=sys.stderr)
        return 1
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
