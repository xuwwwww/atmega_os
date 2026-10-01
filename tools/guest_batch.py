#!/usr/bin/env python3
"""Build selected C files as independent named guests and upload each one."""

import argparse
import glob
import os
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--sources", nargs="+", required=True)
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    selected = []
    for pattern in args.sources:
        matches = glob.glob(pattern)
        selected.extend(matches if matches else [pattern])
    selected = sorted(dict.fromkeys(os.path.abspath(path) for path in selected))

    if not selected:
        raise SystemExit("no guest C source selected")
    if len(selected) > 3:
        raise SystemExit(
            f"selected {len(selected)} guests, but this Uno build has three "
            "resident VM slots"
        )

    guests = []
    for source in selected:
        if not os.path.isfile(source) or not source.endswith(".c"):
            raise SystemExit(f"not a C source file: {source}")
        name = os.path.splitext(os.path.basename(source))[0]
        if not 1 <= len(name) <= 11 or any(
            character not in "abcdefghijklmnopqrstuvwxyz0123456789_-"
            for character in name
        ):
            raise SystemExit(
                f"invalid guest name {name!r}: use 1..11 lowercase letters, "
                "digits, '_' or '-'"
            )

        subprocess.run(
            [
                "make", "-C", os.path.join(repo, "guest-riscv"),
                f"NAME={name}", f"MAIN={source}", "all",
            ],
            check=True,
        )
        binary = os.path.join(repo, "guest-riscv", "build", f"{name}.bin")
        guests.append((name, binary))

    command = [
        "make", "-C", repo, "guest-load-batch",
        "GUEST_BATCH=" + " ".join(
            f"--guest {name}={binary}" for name, binary in guests
        ),
    ]
    subprocess.run(command, check=True)

    print("uploaded guests: " + ", ".join(
        os.path.splitext(os.path.basename(path))[0] for path in selected
    ))


if __name__ == "__main__":
    main()
