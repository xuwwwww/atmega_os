#!/usr/bin/env python3
"""Build selected C files as independent named guests and upload each one."""

import argparse
import glob
import json
import os
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--sources", nargs="+", required=True)
    parser.add_argument(
        "--build-only", action="store_true",
        help="keep images in the PC build directory for demand loading",
    )
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    selected = []
    for pattern in args.sources:
        matches = glob.glob(pattern)
        selected.extend(matches if matches else [pattern])
    selected = sorted(dict.fromkeys(os.path.abspath(path) for path in selected))

    if not selected:
        raise SystemExit("no guest C source selected")
    if not args.build_only and len(selected) > 3:
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

    if args.build_only:
        build_directory = os.path.join(repo, "guest-riscv", "build")
        manifest_path = os.path.join(build_directory, "library.json")
        manifest = {
            "version": 1,
            "images": {
                name: os.path.basename(binary) for name, binary in guests
            },
        }
        with open(manifest_path, "w", encoding="utf-8") as manifest_file:
            json.dump(manifest, manifest_file, indent=2, sort_keys=True)
            manifest_file.write("\n")
        print("PC guest library: " + ", ".join(
            os.path.splitext(os.path.basename(path))[0] for path in selected
        ))
        print("library manifest: " + manifest_path)
        return

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
