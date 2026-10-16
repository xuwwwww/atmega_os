#!/usr/bin/env python3
"""Relay a WSL terminal to the Windows COM-port monitor without Console buffering."""

import argparse
import glob
import json
import os
import re
import selectors
import subprocess
import sys
import termios
import time
import tty

from generated_log_codec import DECODE_TABLE


FRAME_ESCAPE = 0x00
FRAME_SYNC = 0x7E
FRAME_COMPRESSED = 0x01
FRAME_STACK_REPORT = 0x02
FRAME_SHELL_TEXT = 0x03
FRAME_PANIC = 0x04

BINARY_GUEST_CHUNK_SIZE = 16
BINARY_SYNC = b"\xa5\x5a"
BINARY_OP_DATA = 1
BINARY_OP_COMMIT = 2


def crc16_ccitt(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (((crc << 1) ^ 0x1021) & 0xFFFF
                   if crc & 0x8000 else (crc << 1) & 0xFFFF)
    return crc


def binary_guest_packet(operation, offset=0, payload=b""):
    body = bytes([
        operation, offset & 0xFF, (offset >> 8) & 0xFF, len(payload)
    ]) + payload
    crc = crc16_ccitt(body)
    return BINARY_SYNC + body + bytes([crc & 0xFF, crc >> 8])


class DemandGuestLoader:
    """Serve PC-resident guest images requested by the Uno shell."""

    RUN_REQUEST = re.compile(rb"HOST RUN ([a-z0-9_-]{1,11}) ([0-3])")

    def __init__(self, process, guest_dirs, timeout=3.0, retries=5):
        self.process = process
        self.guest_dirs = guest_dirs
        self.timeout = timeout
        self.retries = retries
        self.buffer = bytearray()
        self.state = "idle"
        self.name = None
        self.priority = 2
        self.image = b""
        self.image_crc = 0
        self.offset = 0
        self.chunk_end = 0
        self.packet = b""
        self.attempt = 0
        self.deadline = 0.0
        self.quiet_protocol = False
        self.quiet_buffer = bytearray()

    @property
    def busy(self):
        return self.state != "idle"

    def _send(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def _find_image(self, name):
        for directory in self.guest_dirs:
            manifest = self._read_manifest(directory)
            if manifest is not None and name in manifest:
                path = os.path.join(directory, manifest[name])
                if os.path.isfile(path):
                    return path
            path = os.path.join(directory, name + ".bin")
            if os.path.isfile(path):
                return path
        return None

    @staticmethod
    def _read_manifest(directory):
        path = os.path.join(directory, "library.json")
        if not os.path.isfile(path):
            return None
        try:
            with open(path, "r", encoding="utf-8") as manifest_file:
                document = json.load(manifest_file)
            images = document.get("images")
            return images if document.get("version") == 1 and isinstance(images, dict) else None
        except (OSError, ValueError):
            return None

    def _print_library(self):
        images = {}
        for directory in self.guest_dirs:
            manifest = self._read_manifest(directory)
            if manifest is not None:
                for name, filename in manifest.items():
                    path = os.path.join(directory, filename)
                    if os.path.isfile(path):
                        images.setdefault(name, path)
            else:
                for path in glob.glob(os.path.join(directory, "*.bin")):
                    images.setdefault(os.path.splitext(os.path.basename(path))[0], path)
        if not images:
            print("[host library] empty; run: make guest-library "
                  "GUEST_CS='guest-riscv/workloads/*.c'")
        else:
            print("[host library] PC-backed RV32 images:")
            for name in sorted(images):
                print(f"  {name:<11} {os.path.getsize(images[name])} bytes")
        sys.stdout.flush()

    def _fail(self, message):
        print(f"\n[host load failed] {message}")
        sys.stdout.flush()
        self.state = "idle"
        self.quiet_protocol = False
        self.quiet_buffer.clear()
        self.buffer.clear()

    def _start(self, name, priority):
        path = self._find_image(name)
        if path is None:
            self._fail(
                f"{name}.bin is not in {', '.join(self.guest_dirs)}; "
                "build it with make guest-library"
            )
            return
        with open(path, "rb") as guest_file:
            image = guest_file.read()
        if not 1 <= len(image) <= 192:
            self._fail(f"{path} is {len(image)} bytes; capacity is 1..192")
            return

        self.name = name
        self.priority = priority
        self.image = image
        self.image_crc = crc16_ccitt(image)
        self.offset = 0
        self.attempt = 0
        self.buffer.clear()
        self.state = "ready"
        self.quiet_protocol = True
        self.quiet_buffer.clear()
        self.deadline = time.monotonic() + self.timeout
        print(f"[loading {name}] 0/{len(image)} bytes (0%) from PC to Uno")
        sys.stdout.flush()
        self._send(
            f"guest bload {name} {len(image)} {self.image_crc:04x} 0\r".encode(
                "ascii"
            )
        )

    def _send_chunk(self):
        chunk = self.image[
            self.offset:self.offset + BINARY_GUEST_CHUNK_SIZE
        ]
        self.chunk_end = self.offset + len(chunk)
        self.packet = binary_guest_packet(BINARY_OP_DATA, self.offset, chunk)
        self.attempt = 1
        self.state = "ack"
        self.deadline = time.monotonic() + self.timeout
        self.buffer.clear()
        self._send(self.packet)

    def _retry_packet(self, reason):
        if self.attempt >= self.retries:
            self._fail(f"{reason}; retry limit reached at byte {self.offset}")
            return
        self.attempt += 1
        self.deadline = time.monotonic() + self.timeout
        self.buffer.clear()
        print(f"[loading {self.name}] retry {self.attempt}/{self.retries} "
              f"at byte {self.offset}")
        sys.stdout.flush()
        self._send(self.packet)

    def feed(self, decoded):
        self.buffer.extend(decoded)
        if len(self.buffer) > 8192:
            del self.buffer[:-4096]

        if self.state == "idle":
            if b"HOST LIST" in self.buffer:
                marker_end = self.buffer.index(b"HOST LIST") + len(b"HOST LIST")
                del self.buffer[:marker_end]
                self._print_library()
            match = self.RUN_REQUEST.search(self.buffer)
            if match:
                name = match.group(1).decode("ascii")
                priority = int(match.group(2))
                self._start(name, priority)
            return

        if b"ATmega OS" in self.buffer:
            self._fail("Uno rebooted during the transfer")
            return
        if self.state == "ready":
            expected = (f"READY name={self.name} size={len(self.image)} "
                        "chunk=16 encoding=binary").encode("ascii")
            if expected in self.buffer:
                self._send_chunk()
            elif b"LOAD ERROR" in self.buffer:
                self._fail("Uno rejected the load session")
            return

        if self.state == "ack":
            expected_crc = crc16_ccitt(self.image[:self.chunk_end])
            expected = (f"BINARY ACK bytes={self.chunk_end} "
                        f"crc={expected_crc:04x}").encode("ascii")
            if expected in self.buffer:
                self.offset = self.chunk_end
                percent = self.offset * 100 // len(self.image)
                print(f"[loading {self.name}] {self.offset}/{len(self.image)} "
                      f"bytes ({percent}%)")
                sys.stdout.flush()
                if self.offset < len(self.image):
                    self._send_chunk()
                else:
                    self.packet = binary_guest_packet(BINARY_OP_COMMIT)
                    self.attempt = 1
                    self.state = "commit"
                    self.deadline = time.monotonic() + self.timeout
                    self.buffer.clear()
                    self._send(self.packet)
            elif b"BINARY NACK" in self.buffer:
                self._retry_packet("Uno returned NACK")
            return

        if self.state == "commit":
            if b"LOAD OK" in self.buffer:
                print(f"[loading {self.name}] complete; starting Uno VM")
                sys.stdout.flush()
                command = f"run-cached {self.name} {self.priority}\r"
                self.state = "idle"
                self.buffer.clear()
                self._send(command.encode("ascii"))
            elif b"LOAD ERROR" in self.buffer or b"BINARY NACK" in self.buffer:
                self._retry_packet("Uno rejected commit")

    def visible_output(self, decoded):
        """Hide loader protocol chatter while preserving progress and VM text."""
        if self.quiet_protocol:
            self.quiet_buffer.extend(decoded)
            marker = b"[RUNNING]"
            position = self.quiet_buffer.find(marker)
            if position < 0:
                if len(self.quiet_buffer) > 4096:
                    del self.quiet_buffer[:-512]
                return b""
            visible = bytes(self.quiet_buffer[position:])
            self.quiet_buffer.clear()
            self.quiet_protocol = False
            return visible

        # HOST LIST is a firmware-to-monitor control request, not user output.
        decoded = decoded.replace(b"HOST LIST\r\n", b"")
        return self.RUN_REQUEST.sub(b"", decoded)

    def poll(self):
        if not self.busy or time.monotonic() < self.deadline:
            return
        if self.state == "ack" or self.state == "commit":
            self._retry_packet("UART response timeout")
        else:
            self._fail("timed out waiting for Uno loader")


class TerminalInputFilter:
    """Pass text plus Up/Down history keys; discard other terminal controls."""

    def __init__(self):
        self.state = "text"
        self.sequence_prefix = None
        self.current_line = bytearray()
        self.history = []
        self.history_index = None

    def _replace_line(self, replacement):
        output = bytearray(b"\b \b" * len(self.current_line))
        output.extend(replacement)
        self.current_line = bytearray(replacement)
        return output

    def _recall(self, direction):
        if not self.history:
            return bytearray()

        if direction == "up":
            if self.history_index is None:
                self.history_index = len(self.history) - 1
            elif self.history_index > 0:
                self.history_index -= 1
            return self._replace_line(self.history[self.history_index])

        if self.history_index is None:
            return bytearray()
        if self.history_index + 1 < len(self.history):
            self.history_index += 1
            return self._replace_line(self.history[self.history_index])
        self.history_index = None
        return self._replace_line(b"")

    def render_history(self):
        return b"".join(
            f"{index} ".encode("ascii") + command + b"\r\n"
            for index, command in enumerate(self.history, 1)
        )

    def feed(self, data):
        output = bytearray()

        for byte in data:
            if self.state == "escape":
                if byte in (ord("["), ord("O")):
                    self.sequence_prefix = byte
                    self.state = "sequence"
                else:
                    self.state = "text"
                continue

            if self.state == "sequence":
                if byte == ord("A"):
                    output.extend(self._recall("up"))
                elif byte == ord("B"):
                    output.extend(self._recall("down"))
                if 0x40 <= byte <= 0x7E:
                    self.state = "text"
                continue

            if byte == 0x1B:
                self.state = "escape"
            elif byte in (0x08, 0x7F):
                if self.current_line:
                    self.current_line.pop()
                self.history_index = None
                output.append(byte)
            elif byte in (0x0A, 0x0D):
                command = bytes(self.current_line)
                if command and (not self.history or self.history[-1] != command):
                    self.history.append(command)
                    del self.history[:-32]
                self.current_line.clear()
                self.history_index = None
                output.append(byte)
            elif 0x20 <= byte <= 0x7E:
                self.current_line.append(byte)
                self.history_index = None
                output.append(byte)

        return bytes(output)


class SerialDecoder:
    def __init__(self, history_provider=None):
        self.state = "raw"
        self.transport_escape = False
        self.bit_count = 0
        self.payload_size = 0
        self.payload = bytearray()
        self.frame_type = None
        self.history_provider = history_provider

    @staticmethod
    def decode_huffman(payload, bit_count):
        output = bytearray()
        code = 0
        code_length = 0

        for bit_index in range(bit_count):
            byte = payload[bit_index // 8]
            bit = (byte >> (7 - bit_index % 8)) & 1
            code = (code << 1) | bit
            code_length += 1

            symbol = DECODE_TABLE.get((code, code_length))
            if symbol is not None:
                output.append(symbol)
                code = 0
                code_length = 0

        if code_length != 0:
            return b"[invalid Huffman frame]"

        return bytes(output)

    def feed(self, data):
        output = bytearray()

        for physical_byte in data:
            if self.transport_escape:
                self.transport_escape = False
                if physical_byte == FRAME_ESCAPE:
                    byte = FRAME_ESCAPE
                elif physical_byte == FRAME_SYNC:
                    # A sync marker is valid in every parser state. Drop any
                    # incomplete frame left behind by corruption or reboot.
                    self.state = "frame_type"
                    self.payload.clear()
                    self.bit_count = 0
                    self.payload_size = 0
                    self.frame_type = None
                    continue
                else:
                    output.extend(b"[UART framing error]")
                    self.state = "raw"
                    continue
            elif physical_byte == FRAME_ESCAPE:
                self.transport_escape = True
                continue
            else:
                byte = physical_byte

            if self.state == "raw":
                output.append(byte)
                continue

            if self.state == "frame_type":
                if byte == FRAME_COMPRESSED:
                    self.state = "length_low"
                elif byte in (FRAME_STACK_REPORT, FRAME_SHELL_TEXT, FRAME_PANIC):
                    self.frame_type = byte
                    self.state = "binary_length"
                else:
                    output.extend(b"[unknown UART frame]")
                    self.state = "raw"
                continue

            if self.state == "length_low":
                self.bit_count = byte
                self.state = "length_high"
                continue

            if self.state == "length_high":
                self.bit_count |= byte << 8
                self.payload_size = (self.bit_count + 7) // 8
                self.payload.clear()
                if self.payload_size == 0:
                    self.state = "raw"
                else:
                    self.state = "payload"
                continue

            if self.state == "binary_length":
                self.payload_size = byte
                self.payload.clear()
                if self.payload_size == 0:
                    self.state = "raw"
                else:
                    self.state = "binary_payload"
                continue

            if self.state == "binary_payload":
                self.payload.append(byte)
                if len(self.payload) == self.payload_size:
                    if self.frame_type == FRAME_STACK_REPORT and len(self.payload) == 16:
                        state_names = {
                            0: "READY",
                            1: "RUNNING",
                            2: "SLEEPING",
                            3: "TERMINATED",
                            4: "BLOCKED",
                        }
                        unused = ", ".join(str(value) for value in self.payload[:4])
                        states = ", ".join(
                            state_names.get(value, f"UNKNOWN({value})")
                            for value in self.payload[4:8]
                        )
                        priorities = ", ".join(
                            f"{base}/{effective}"
                            for base, effective in zip(
                                self.payload[8:12], self.payload[12:16]
                            )
                        )
                        message = (
                            f"[stack unused: {unused}; states: {states}; "
                            f"priorities base/effective: {priorities}]\r\n"
                        )
                    elif self.frame_type == FRAME_SHELL_TEXT:
                        if (self.payload == b"\x1fH" and
                                self.history_provider is not None):
                            output.extend(self.history_provider())
                        else:
                            output.extend(self.payload)
                            output.extend(b"\r\n")
                        self.state = "raw"
                        continue
                    elif self.frame_type == FRAME_PANIC and len(self.payload) == 2:
                        message = (
                            f"\r\n[KERNEL PANIC code={self.payload[0]} "
                            f"detail={self.payload[1]}; rebooting]\r\n"
                        )
                    else:
                        values = ", ".join(str(value) for value in self.payload)
                        message = f"[binary frame {self.frame_type}: {values}]\r\n"
                    output.extend(message.encode("ascii"))
                    self.state = "raw"
                continue

            self.payload.append(byte)
            if len(self.payload) == self.payload_size:
                output.extend(self.decode_huffman(self.payload, self.bit_count))
                self.state = "raw"

        return bytes(output)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--guest-dir", action="append", default=[],
        help="directory containing PC-backed NAME.bin guest images",
    )
    parser.add_argument("--reset", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    input_filter = TerminalInputFilter()
    decoder = SerialDecoder(input_filter.render_history)
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
    if args.reset:
        command.append("-ResetOnOpen")

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    guest_dirs = [os.path.abspath(path) for path in args.guest_dir]
    demand_loader = DemandGuestLoader(process, guest_dirs)

    stdin_fd = sys.stdin.fileno()
    stdout_fd = process.stdout.fileno()
    old_terminal = termios.tcgetattr(stdin_fd) if sys.stdin.isatty() else None
    selector = selectors.DefaultSelector()
    selector.register(stdout_fd, selectors.EVENT_READ, "serial")
    selector.register(stdin_fd, selectors.EVENT_READ, "keyboard")
    user_exit = False
    # The first CR is sent by the Windows relay immediately after Open().  A
    # second one is delayed until an Uno bootloader/reset window has elapsed;
    # this guarantees that a successful interactive connection visibly lands
    # on the application shell prompt.
    prompt_deadline = time.monotonic() + 2.0
    prompt_requested = False

    sys.stdout.write(
        f"Serial monitor: {args.port} at {args.baud} baud "
        "(Ctrl+] or Ctrl+C to exit)\n"
    )
    sys.stdout.flush()

    try:
        if old_terminal is not None:
            tty.setcbreak(stdin_fd)

        while process.poll() is None:
            demand_loader.poll()
            if not prompt_requested and time.monotonic() >= prompt_deadline:
                process.stdin.write(b"\r")
                process.stdin.flush()
                prompt_requested = True
            for key, _ in selector.select(timeout=0.05):
                if key.data == "serial":
                    data = os.read(stdout_fd, 4096)
                    if not data:
                        return process.wait()
                    decoded = decoder.feed(data)
                    if decoded:
                        demand_loader.feed(decoded)
                        visible = demand_loader.visible_output(decoded)
                        if visible:
                            os.write(sys.stdout.fileno(), visible)
                    continue

                data = os.read(stdin_fd, 256)
                if not data:
                    user_exit = True
                    return 0

                if b"\x1d" in data:
                    before_exit = data.split(b"\x1d", 1)[0]
                    filtered = input_filter.feed(before_exit)
                    if filtered:
                        process.stdin.write(filtered)
                        process.stdin.flush()
                    user_exit = True
                    return 0

                filtered = input_filter.feed(data)
                if filtered:
                    if demand_loader.busy:
                        os.write(
                            sys.stdout.fileno(),
                            b"\r\n[host loader busy; input ignored]\r\n",
                        )
                    else:
                        process.stdin.write(filtered)
                        process.stdin.flush()

        return process.returncode
    except KeyboardInterrupt:
        user_exit = True
        return 0
    finally:
        selector.close()
        if old_terminal is not None:
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_terminal)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if user_exit:
            sys.stdout.write("\n")
            sys.stdout.flush()


if __name__ == "__main__":
    raise SystemExit(main())
