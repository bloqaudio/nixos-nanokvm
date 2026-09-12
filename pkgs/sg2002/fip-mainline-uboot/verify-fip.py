#!/usr/bin/env python3
"""Validate the SG2002 PARAM2 payload map and optional C906L image."""

import argparse
import binascii
import struct
from pathlib import Path


def u32(blob: bytes, offset: int, name: str) -> int:
    if offset + 4 > len(blob):
        raise ValueError(f"{name} at {offset:#x} is outside the FIP")
    return struct.unpack_from("<I", blob, offset)[0]


def padded(payload: bytes) -> bytes:
    return payload + bytes(-len(payload) % 512)


def checksum(payload: bytes) -> bytes:
    return struct.pack("<H", binascii.crc_hqx(payload, 0)) + b"\xfe\xca"


def contained(blob: bytes, start: int, size: int, name: str) -> bytes:
    end = start + size
    if start < 0 or end < start or end > len(blob):
        raise ValueError(f"{name} extent [{start:#x}, {end:#x}) is outside the FIP")
    return blob[start:end]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("fip", type=Path)
    parser.add_argument("--rtos", type=Path)
    parser.add_argument("--rtos-runaddr", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--rtos-contract-sha256")
    args = parser.parse_args()

    if bool(args.rtos) != bool(args.rtos_contract_sha256):
        raise ValueError(
            "--rtos and --rtos-contract-sha256 must be supplied together"
        )
    expected_digest = b""
    if args.rtos_contract_sha256:
        try:
            expected_digest = bytes.fromhex(args.rtos_contract_sha256)
        except ValueError as exc:
            raise ValueError("C906L contract SHA-256 is not hexadecimal") from exc
        if len(expected_digest) != 32:
            raise ValueError("C906L contract SHA-256 is not exactly 32 bytes")

    blob = args.fip.read_bytes()
    if not blob.startswith(b"CVBL01"):
        raise ValueError("PARAM1 magic is missing")
    p2_offset = u32(blob, 224, "PARAM1.PARAM2_LOADADDR")
    if contained(blob, p2_offset, 8, "PARAM2 magic")[:6] != b"CVLD02":
        raise ValueError("PARAM2 magic is missing")

    ddr_address = u32(blob, p2_offset + 20, "PARAM2.DDR_PARAM_LOADADDR")
    ddr_size = u32(blob, p2_offset + 24, "PARAM2.DDR_PARAM_SIZE")
    rtos_address = u32(blob, p2_offset + 36, "PARAM2.BLCP_2ND_LOADADDR")
    rtos_size = u32(blob, p2_offset + 40, "PARAM2.BLCP_2ND_SIZE")
    rtos_runaddr = u32(blob, p2_offset + 44, "PARAM2.BLCP_2ND_RUNADDR")
    monitor_address = u32(blob, p2_offset + 52, "PARAM2.MONITOR_LOADADDR")

    contained(blob, ddr_address, ddr_size, "DDR parameters")
    actual_rtos = contained(blob, rtos_address, rtos_size, "C906L firmware")
    if rtos_address != ddr_address + ddr_size:
        raise ValueError("C906L payload does not immediately follow DDR parameters")
    if monitor_address != rtos_address + rtos_size:
        raise ValueError("OpenSBI payload does not immediately follow C906L firmware")
    if rtos_runaddr != args.rtos_runaddr:
        raise ValueError(
            f"C906L run address is {rtos_runaddr:#x}, expected {args.rtos_runaddr:#x}"
        )

    raw_rtos = b"" if args.rtos is None else args.rtos.read_bytes()
    expected_rtos = padded(raw_rtos)
    if actual_rtos != expected_rtos:
        raise ValueError("packed C906L firmware differs from its input")
    if expected_digest and expected_digest not in raw_rtos:
        raise ValueError(
            "C906L firmware does not contain its declared semantic contract SHA-256"
        )
    if blob[p2_offset + 32 : p2_offset + 36] != checksum(expected_rtos):
        raise ValueError("C906L firmware checksum is invalid")
    if bool(expected_rtos) != bool(rtos_runaddr):
        raise ValueError("C906L payload and run address must either both be present or both absent")

    print(
        f"validated {args.fip}: C906L size={rtos_size:#x} "
        f"runaddr={rtos_runaddr:#x}"
    )


if __name__ == "__main__":
    main()
