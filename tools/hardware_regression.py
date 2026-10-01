#!/usr/bin/env python3
"""Run identical RV32 workloads on multiple Arduino hardware targets."""

import argparse
import os
import re
import subprocess
import sys

from wsl_guest_upload import crc16_ccitt


WORKLOADS = {
    "bitwalk": 39,
    "checksum": 8,
    "fibonacci": 64,
}


def run_visible(command, cwd):
    """Stream a child command while retaining its complete decoded output."""
    print("\n$ " + " ".join(command), flush=True)
    process = subprocess.Popen(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        bufsize=1,
    )
    captured = []
    assert process.stdout is not None
    for line in process.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        captured.append(line)
    return process.wait(), "".join(captured)


def build_workloads(repo):
    images = {}
    for name in WORKLOADS:
        source = os.path.join(repo, "guest-riscv", "workloads", name + ".c")
        command = [
            "make", "-C", os.path.join(repo, "guest-riscv"),
            f"NAME={name}", f"MAIN={source}", "all",
        ]
        subprocess.run(command, check=True)
        path = os.path.join(repo, "guest-riscv", "build", name + ".bin")
        with open(path, "rb") as image_file:
            image = image_file.read()
        images[name] = (path, len(image), crc16_ccitt(image))
    return images


def assess_output(return_code, output, images):
    exits = {
        name: int(value)
        for name, value in re.findall(
            r"^rv32 ([a-z0-9_-]+) exit=(\d+)\s*$", output, re.MULTILINE
        )
    }
    faults = {
        name: (int(code), int(pc))
        for name, code, pc in re.findall(
            r"^rv32 ([a-z0-9_-]+) fault=(\d+) pc=(\d+)\s*$",
            output,
            re.MULTILINE,
        )
    }
    integrity = {
        name: (int(pointer_ok), int(size), actual.lower(), expected.lower())
        for name, pointer_ok, size, actual, expected in re.findall(
            r"^verify ([a-z0-9_-]+) ptr=(\d+) size=(\d+) "
            r"crc=([0-9a-fA-F]{4})/([0-9a-fA-F]{4})\s*$",
            output,
            re.MULTILINE,
        )
    }

    reasons = []
    if return_code != 0:
        reasons.append(f"runner exited {return_code}")
    for name, expected_exit in WORKLOADS.items():
        if name in faults:
            code, pc = faults[name]
            reasons.append(f"{name} fault={code} pc={pc}")
        elif name not in exits:
            reasons.append(f"{name} result missing")
        elif exits[name] != expected_exit:
            reasons.append(
                f"{name} exit={exits[name]} expected={expected_exit}"
            )

        expected_crc = f"{images[name][2]:04x}"
        if name not in integrity:
            reasons.append(f"{name} integrity record missing")
        else:
            pointer_ok, memory_size, actual_crc, recorded_crc = integrity[name]
            if pointer_ok != 1 or memory_size != 192:
                reasons.append(
                    f"{name} VM memory ptr={pointer_ok} size={memory_size}"
                )
            if actual_crc != expected_crc or recorded_crc != expected_crc:
                reasons.append(
                    f"{name} crc={actual_crc}/{recorded_crc} "
                    f"expected={expected_crc}"
                )
    return not reasons, exits, faults, reasons


def test_port(repo, port, images, flash, run_timeout):
    if flash:
        flash_code, flash_output = run_visible(
            ["make", "-C", repo, "upload", f"PORT={port}"], repo
        )
        if flash_code != 0:
            return False, {}, {}, [f"flash failed ({flash_code})"], flash_output

    guest_arguments = []
    for name, (path, _size, _crc) in images.items():
        guest_arguments.extend(["--guest", f"{name}={path}"])

    command = [
        "make", "-C", repo, "guest-load-batch", f"PORT={port}",
        "GUEST_BATCH=" + " ".join(guest_arguments),
        "GUEST_UPLOAD_FLAGS="
        f"--run-all --prefix --fail-on-reboot --run-timeout {run_timeout}",
    ]
    return_code, output = run_visible(command, repo)
    passed, exits, faults, reasons = assess_output(return_code, output, images)
    return passed, exits, faults, reasons, output


def parse_args():
    parser = argparse.ArgumentParser(
        description="Flash/upload/run identical workloads on several boards"
    )
    parser.add_argument("--repo", required=True)
    parser.add_argument("--ports", nargs="+", required=True)
    parser.add_argument("--expected-unstable", nargs="*", default=[])
    parser.add_argument("--flash", action="store_true")
    parser.add_argument("--run-timeout", type=int, default=120)
    return parser.parse_args()


def main():
    args = parse_args()
    repo = os.path.abspath(args.repo)
    unstable = {port.upper() for port in args.expected_unstable}
    images = build_workloads(repo)
    results = []

    for port in args.ports:
        passed, exits, faults, reasons, _output = test_port(
            repo, port, images, args.flash, args.run_timeout
        )
        if port.upper() in unstable:
            status = "XPASS" if passed else "XFAIL"
        else:
            status = "PASS" if passed else "FAIL"
        results.append((port, status, exits, faults, reasons))

    print("\nHardware regression summary")
    print("port  status  bitwalk  checksum  fibonacci  details")
    for port, status, exits, faults, reasons in results:
        values = []
        for name in WORKLOADS:
            if name in faults:
                values.append(f"F{faults[name][0]}@{faults[name][1]}")
            elif name in exits:
                values.append(str(exits[name]))
            else:
                values.append("-")
        print(
            f"{port:<5} {status:<6} {values[0]:<8} {values[1]:<9} "
            f"{values[2]:<10} {'; '.join(reasons) if reasons else '-'}"
        )

    unexpected_failures = [
        port for port, status, _exits, _faults, _reasons in results
        if status == "FAIL"
    ]
    if unexpected_failures:
        print(
            "\nunexpected stable-board failures: "
            + ", ".join(unexpected_failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
