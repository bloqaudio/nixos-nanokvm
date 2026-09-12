#!/usr/bin/env python3
"""Verify that a Rust staticlib and its target specification are wholly lp64d."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*args: str, env: dict[str, str] | None = None) -> bytes:
    try:
        return subprocess.check_output(args, env=env, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as error:
        output = error.output.decode(errors="replace").strip()
        fail(f"{args[0]!r} exited with status {error.returncode}: {output}")


def fail(message: str) -> None:
    raise SystemExit(f"Rust staticlib verification failed: {message}")


def verify_target(rustc: str, target: str) -> None:
    rustc_env = os.environ.copy()
    # Printing the built-in target as JSON remains an unstable diagnostic, but
    # does not opt the firmware build itself into unstable Rust features.
    rustc_env["RUSTC_BOOTSTRAP"] = "1"
    raw_spec = run(
        rustc,
        "--print",
        "target-spec-json",
        "-Z",
        "unstable-options",
        "--target",
        target,
        env=rustc_env,
    )
    try:
        spec = json.loads(raw_spec)
    except json.JSONDecodeError as error:
        fail(f"rustc returned invalid target JSON: {error}")

    if spec.get("arch") != "riscv64":
        fail(f"target arch is {spec.get('arch')!r}, expected 'riscv64'")
    if spec.get("llvm-abiname") != "lp64d":
        fail(
            f"target LLVM ABI is {spec.get('llvm-abiname')!r}, expected 'lp64d'"
        )
    if spec.get("code-model") != "medium":
        fail(
            f"target code model is {spec.get('code-model')!r}, expected 'medium'"
        )

    features = set(spec.get("features", "").split(","))
    missing_features = {"+f", "+d"} - features
    if missing_features:
        fail(f"target lacks required features: {', '.join(sorted(missing_features))}")


def verify_archive(archive: Path, ar: str, readelf: str) -> int:
    members = run(ar, "t", str(archive)).decode().splitlines()
    if not members:
        fail("archive contains no members")
    if len(members) != len(set(members)):
        fail(
            "archive contains duplicate member names and cannot be audited "
            "unambiguously"
        )

    with tempfile.TemporaryDirectory() as temporary:
        object_path = Path(temporary) / "member.o"
        for member in members:
            object_path.write_bytes(run(ar, "p", str(archive), member))
            header = run(readelf, "--file-header", str(object_path)).decode()
            if "Class:" not in header or "ELF64" not in header:
                fail(f"{member!r} is not an ELF64 object")
            if "Machine:" not in header or "RISC-V" not in header:
                fail(f"{member!r} is not a RISC-V object")
            if "double-float ABI" not in header:
                flags = next(
                    (line.strip() for line in header.splitlines() if "Flags:" in line),
                    "missing Flags header",
                )
                fail(f"{member!r} is not lp64d ({flags})")

    return len(members)


def main() -> None:
    if len(sys.argv) != 6:
        fail("usage: verify-staticlib.py ARCHIVE AR READELF RUSTC TARGET")

    archive = Path(sys.argv[1])
    ar, readelf, rustc, target = sys.argv[2:]
    verify_target(rustc, target)
    member_count = verify_archive(archive, ar, readelf)
    print(f"verified {member_count} lp64d Rust staticlib members for {target}")


if __name__ == "__main__":
    main()
