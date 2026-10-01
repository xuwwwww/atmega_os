#!/usr/bin/env python3
"""Relay a WSL terminal to the Windows COM-port monitor without Console buffering."""

import argparse
import os
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
                        os.write(sys.stdout.fileno(), decoded)
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
