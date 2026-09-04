#!/usr/bin/env python3
"""USB bring-up for SG2002 / LicheeRV Nano using mainline U-Boot.

Mainline U-Boot has no cvi_utask (that's a Cvitek vendor command). We use
its built-in `fastboot` gadget instead, and rely on the fact that the
USB-loaded U-Boot FIP uses:

    CONFIG_BOOTCOMMAND="fastboot usb 0"

So once U-Boot starts it falls straight into the fastboot gadget, even
with a bootable SD card inserted — no UART required. Flow:

  1. Push FIP via cv181x-rom-dl (no UART interaction — ROM USB download
     only uses the CVITEK USB Com Port, not the board's debug UART).
  2. On the tested USB BootROM path the FSBL loads and starts C906L, then
     OpenSBI → U-Boot runs and fastboot usb 0 enumerates as VID 18d1:d00d.
  3. With a C906L-bearing FIP, choose the handoff from the observed reset
     state.  A core started after this invocation's FIP transfer is accepted
     only after two stable ABI 1.1 status/manifest snapshots exactly match the
     packaged contract and their heartbeat advances.  A held-reset core also
     gets an exact payload CRC before the vendor reset/vector release sequence.
     Attaching to a core started by an earlier invocation requires explicit
     --accept-running-c906l consent and the same identity/liveness proof; its
     now-mutable payload cannot be CRC-verified.  This runner never activates a
     peripheral lease: lease profiles must still report DORMANT before Linux.
  4. Optional diagnostic stop: `--uboot-only` leaves U-Boot in fastboot
     so the host can issue `fastboot oem run:<cmd>` commands.
  5. Host: `fastboot stage <FIT>` pushes the image to $fastboot_buf_addr.
  6. Host: soft-disconnect U-Boot's DWC2 gadget, then run
     `bootm <addr>`. The disconnect is explicit because a successful
     bootm never returns to fastboot's normal gadget teardown.

Requires `fastboot` on PATH (android-tools).
"""
import argparse
from dataclasses import dataclass
import errno
import os
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

import sg2002_c906l_contract as c906l_contract


FASTBOOT_BUF_ADDR = 0x82000000

# SG2002 DWC2 device-control register.  A successful `bootm` from inside a
# fastboot OEM command never returns through U-Boot's normal gadget cleanup,
# so leave the bus electrically detached until Linux binds its own gadget.
DWC2_DCTL_ADDR = 0x04340804
DWC2_DCTL_SFTDISCON = 1 << 1

# U-Boot's built-in fastboot gadget uses Google's reference VID:PID.
# Same IDs as Android phones in bootloader mode — without a filter,
# `fastboot devices` would happily pick a connected phone and we'd
# stage a NanoKVM FIT into its boot partition. Bad day.
FASTBOOT_VENDOR_ID = 0x18d1
FASTBOOT_PRODUCT_ID = 0xd00d

# Cvitek's BootROM CDC ACM gadget.  Do not start an attempt's transfer
# timeout until this device has actually appeared: an unattended runner can
# be armed minutes before somebody resets the board into ROM mode.
ROM_VENDOR_ID = 0x3346
ROM_PRODUCT_ID = 0x1000

# The SG2002 has 256 MiB of DRAM at 0x80000000.  C906L payload, status and
# cache-eviction reads must stay in that aperture.  Restricting these inputs is
# deliberate: the runner accepts no arbitrary MMIO range from its command
# line, even though U-Boot's OEM command is capable of accessing one.
SG2002_DRAM_START = 0x80000000
SG2002_DRAM_END = 0x90000000

C906L_RESET_REG = c906l_contract.RESET_ADDRESS
C906L_RESET_BIT = c906l_contract.RESET_MASK
C906L_SEC_SYS_REG = c906l_contract.SECURITY_ENABLE_ADDRESS
C906L_SEC_ENABLE_BIT = c906l_contract.SECURITY_ENABLE_MASK
C906L_VECTOR_LOW_REG = c906l_contract.VECTOR_LOW_ADDRESS
C906L_VECTOR_HIGH_REG = c906l_contract.VECTOR_HIGH_ADDRESS

C906L_STATUS_SIZE = c906l_contract.STATUS_SIZE
C906L_STATUS_MAGIC = c906l_contract.SHMEM_MAGIC
C906L_ABI_MAJOR = c906l_contract.ABI_MAJOR
C906L_ABI_MINOR = c906l_contract.ABI_MINOR
C906L_STATE_RUNNING = c906l_contract.STATE_RUNNING
C906L_STATE_FAULT = c906l_contract.STATE_FAULT
C906L_CAP_SHMEM_HEARTBEAT = c906l_contract.CAP_SHMEM_HEARTBEAT
C906L_MANIFEST_SIZE = c906l_contract.MANIFEST_SIZE
C906L_SNAPSHOT_SIZE = C906L_STATUS_SIZE + C906L_MANIFEST_SIZE
C906L_CONTRACT_SHA256 = bytes.fromhex(c906l_contract.CONTRACT_SHA256)
C906L_CAPABILITY_WIRE_WIDTH = c906l_contract.CAPABILITY_WIRE_WIDTH
C906L_MIN_CACHE_SCRATCH_SIZE = 1024 * 1024

if (C906L_ABI_MAJOR, C906L_ABI_MINOR) != (1, 1):
    raise RuntimeError(
        "usb-boot-mainline supports exactly the SG2002 C906L ABI 1.1 contract"
    )
if c906l_contract.MANIFEST_ADDRESS != (
        c906l_contract.SHMEM_ADDRESS + C906L_STATUS_SIZE):
    raise RuntimeError("C906L status and manifest are not one contiguous snapshot")

_CRC32_LINE = re.compile(
    r"CRC32 for\s+([0-9a-fA-F]+)\s+\.\.\.\s+"
    r"([0-9a-fA-F]+)\s+==>\s+([0-9a-fA-F]{8})",
    re.IGNORECASE,
)
_MD_LONG_LINE = re.compile(
    r"(?im)^(?:\(bootloader\)\s*)?([0-9a-fA-F]{8,16}):\s+"
    r"((?:[0-9a-fA-F]{8}(?:\s+|$)){1,4})"
)


_SERIAL_AUTO = "<auto>"  # sentinel: device present but iSerial empty

# cv181x-rom-dl scans for the first CVITEK ROM gadget and has no usable
# physical-port selector.  Two launchers on one host therefore cannot safely
# program different boards (or the same board): whichever process wins each
# re-enumeration advances the ROM state underneath the other.  An abstract
# Unix socket gives the complete mainline handoff a host-wide, crash-released
# claim without leaving a stale lock file behind.
_ROM_LOCK_ADDRESS = "\0nanokvm-sg2002-usb-boot-mainline"


class C906LBringupError(RuntimeError):
    """A checked C906L handoff invariant was not satisfied."""


@dataclass(frozen=True)
class C906LStatus:
    magic: int
    abi_major: int
    abi_minor: int
    struct_size: int
    state: int
    generation: int
    flags: int
    heartbeat: int
    capabilities: int
    last_request: int
    last_response: int
    activation_state: int
    activation_error: int
    activation_attempts: int
    activation_request_id: int


@dataclass(frozen=True)
class C906LManifest:
    magic: int
    format_major: int
    format_minor: int
    struct_size: int
    generation: int
    contract_epoch: int
    profile_id: int
    abi_major: int
    abi_minor: int
    capability_width: int
    lease_width: int
    final_capabilities: int
    dormant_capabilities: int
    lease_mask: int
    flags: int
    reserved0: int
    contract_sha256: bytes
    reserved1: bytes
    commit: int


@dataclass(frozen=True)
class C906LSnapshot:
    status: C906LStatus
    manifest: C906LManifest


def parse_int(value):
    """argparse integer accepting the conventional 0x-prefixed form."""
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from exc


def ranges_overlap(start_a, size_a, start_b, size_b):
    return start_a < start_b + size_b and start_b < start_a + size_a


def validate_c906l_layout(firmware_size, run_address, shmem_address,
                          scratch_address, scratch_size):
    ranges = {
        "firmware": (run_address, firmware_size),
        "status and manifest": (shmem_address, C906L_SNAPSHOT_SIZE),
        "cache scratch": (scratch_address, scratch_size),
    }
    if firmware_size <= 0:
        raise C906LBringupError("C906L firmware is empty")
    if scratch_size < C906L_MIN_CACHE_SCRATCH_SIZE:
        raise C906LBringupError(
            "C906L cache scratch must be at least "
            f"0x{C906L_MIN_CACHE_SCRATCH_SIZE:x} bytes"
        )
    for name, (address, size) in ranges.items():
        if address % 64:
            raise C906LBringupError(
                f"{name} address 0x{address:x} is not cache-line aligned"
            )
        if size <= 0 or address < SG2002_DRAM_START or address + size > SG2002_DRAM_END:
            raise C906LBringupError(
                f"{name} range 0x{address:x}..0x{address + size:x} "
                "is outside SG2002 DRAM"
            )
    names = list(ranges)
    for index, first in enumerate(names):
        for second in names[index + 1:]:
            a_start, a_size = ranges[first]
            b_start, b_size = ranges[second]
            if ranges_overlap(a_start, a_size, b_start, b_size):
                raise C906LBringupError(
                    f"{first} and {second} ranges overlap"
                )


def select_c906l_handoff_mode(reset_value, fip_transfer_attempted,
                              accept_running):
    """Choose the only safe verification path for the observed reset state."""
    if not reset_value & C906L_RESET_BIT:
        return "held-reset"
    if fip_transfer_attempted:
        return "fsbl-started"
    if accept_running:
        return "explicit-attach"
    raise C906LBringupError(
        "C906L is already out of reset without a FIP transfer by this "
        "runner; refusing an ambiguous attach (use "
        "--accept-running-c906l only after independently proving the image)"
    )


def parse_uboot_crc32(output, expected_address, expected_size):
    matches = []
    for match in _CRC32_LINE.finditer(output):
        start, end, checksum = (int(part, 16) for part in match.groups())
        if start == expected_address and end == expected_address + expected_size - 1:
            matches.append(checksum)
    if len(matches) != 1:
        raise C906LBringupError(
            "expected exactly one matching U-Boot CRC32 result for "
            f"0x{expected_address:x}+0x{expected_size:x}, found {len(matches)}"
        )
    return matches[0]


def parse_uboot_words(output, expected_address, count):
    words = {}
    for match in _MD_LONG_LINE.finditer(output):
        line_address = int(match.group(1), 16)
        for offset, value in enumerate(match.group(2).split()):
            words[line_address + offset * 4] = int(value, 16)
    missing = [expected_address + offset * 4 for offset in range(count)
               if expected_address + offset * 4 not in words]
    if missing:
        raise C906LBringupError(
            f"U-Boot memory dump omitted 0x{missing[0]:x}"
        )
    return [words[expected_address + offset * 4] for offset in range(count)]


def decode_c906l_status(words):
    if len(words) != C906L_STATUS_SIZE // 4:
        raise C906LBringupError("C906L status dump is not exactly 64 bytes")
    return C906LStatus(
        magic=words[0],
        abi_major=words[1] & 0xffff,
        abi_minor=words[1] >> 16,
        struct_size=words[2],
        state=words[3],
        generation=words[4],
        flags=words[5],
        heartbeat=words[6] | words[7] << 32,
        capabilities=words[8] | words[9] << 32,
        last_request=words[10] | words[11] << 32,
        last_response=words[12] | words[13] << 32,
        activation_state=words[14] & 0xff,
        activation_error=(words[14] >> 8) & 0xff,
        activation_attempts=words[14] >> 16,
        activation_request_id=words[15],
    )


def decode_c906l_manifest(words):
    if len(words) != C906L_MANIFEST_SIZE // 4:
        raise C906LBringupError("C906L manifest dump is not exactly 128 bytes")
    return C906LManifest(
        magic=words[0],
        format_major=words[1] & 0xffff,
        format_minor=words[1] >> 16,
        struct_size=words[2],
        generation=words[3],
        contract_epoch=words[4],
        profile_id=words[5],
        abi_major=words[6] & 0xffff,
        abi_minor=words[6] >> 16,
        capability_width=words[7] & 0xffff,
        lease_width=words[7] >> 16,
        final_capabilities=words[8] | words[9] << 32,
        dormant_capabilities=words[10] | words[11] << 32,
        lease_mask=words[12] | words[13] << 32,
        flags=words[14],
        reserved0=words[15],
        contract_sha256=struct.pack("<8I", *words[16:24]),
        reserved1=struct.pack("<7I", *words[24:31]),
        commit=words[31],
    )


def decode_c906l_snapshot(words):
    if len(words) != C906L_SNAPSHOT_SIZE // 4:
        raise C906LBringupError("C906L snapshot has the wrong size")
    status_words = C906L_STATUS_SIZE // 4
    return C906LSnapshot(
        status=decode_c906l_status(words[:status_words]),
        manifest=decode_c906l_manifest(words[status_words:]),
    )


def _require_exact(name, actual, expected):
    if actual != expected:
        if isinstance(actual, int) and isinstance(expected, int):
            detail = f"0x{actual:x}, expected 0x{expected:x}"
        elif isinstance(actual, bytes) and isinstance(expected, bytes):
            detail = f"{actual.hex()}, expected {expected.hex()}"
        else:
            detail = f"{actual!r}, expected {expected!r}"
        raise C906LBringupError(f"C906L {name} is {detail}")


def validate_c906l_snapshot(snapshot):
    """Require one complete ABI 1.1 status/manifest identity snapshot."""
    status = snapshot.status
    manifest = snapshot.manifest

    _require_exact("status magic", status.magic, C906L_STATUS_MAGIC)
    _require_exact("status ABI major", status.abi_major, C906L_ABI_MAJOR)
    _require_exact("status ABI minor", status.abi_minor, C906L_ABI_MINOR)
    _require_exact("status size", status.struct_size, C906L_STATUS_SIZE)
    _require_exact("status state", status.state, C906L_STATE_RUNNING)
    if status.generation == 0:
        raise C906LBringupError("C906L status generation is zero")
    _require_exact("status flags", status.flags, 0)

    expected_capabilities = (
        c906l_contract.DORMANT_CAPABILITIES
        if c906l_contract.ACTIVATION_REQUIRED
        else c906l_contract.EXPECTED_CAPABILITIES
    )
    expected_activation_state = (
        c906l_contract.ACTIVATION_STATE_DORMANT
        if c906l_contract.ACTIVATION_REQUIRED
        else c906l_contract.ACTIVATION_STATE_ACTIVE
    )
    _require_exact("status capabilities", status.capabilities,
                   expected_capabilities)
    _require_exact("activation state", status.activation_state,
                   expected_activation_state)
    _require_exact("activation error", status.activation_error,
                   c906l_contract.ACTIVATION_RESULT_SUCCESS)
    _require_exact("activation attempts", status.activation_attempts, 0)
    _require_exact("activation request ID", status.activation_request_id, 0)

    _require_exact("manifest magic", manifest.magic,
                   c906l_contract.MANIFEST_MAGIC)
    _require_exact("manifest format major", manifest.format_major,
                   c906l_contract.MANIFEST_FORMAT_MAJOR)
    _require_exact("manifest format minor", manifest.format_minor,
                   c906l_contract.MANIFEST_FORMAT_MINOR)
    _require_exact("manifest size", manifest.struct_size,
                   c906l_contract.MANIFEST_SIZE)
    _require_exact("manifest generation", manifest.generation,
                   status.generation)
    _require_exact("contract epoch", manifest.contract_epoch,
                   c906l_contract.CONTRACT_EPOCH)
    _require_exact("profile ID", manifest.profile_id,
                   c906l_contract.PROFILE_ID)
    _require_exact("manifest ABI major", manifest.abi_major,
                   C906L_ABI_MAJOR)
    _require_exact("manifest ABI minor", manifest.abi_minor,
                   C906L_ABI_MINOR)
    _require_exact("capability wire width", manifest.capability_width,
                   C906L_CAPABILITY_WIRE_WIDTH)
    _require_exact("lease wire width", manifest.lease_width,
                   c906l_contract.LEASE_WIRE_WIDTH)
    _require_exact("final capabilities", manifest.final_capabilities,
                   c906l_contract.EXPECTED_CAPABILITIES)
    _require_exact("dormant capabilities", manifest.dormant_capabilities,
                   c906l_contract.DORMANT_CAPABILITIES)
    _require_exact("lease mask", manifest.lease_mask,
                   c906l_contract.LEASE_MASK)
    _require_exact("manifest flags", manifest.flags,
                   c906l_contract.MANIFEST_FLAGS)
    _require_exact("manifest reserved0", manifest.reserved0, 0)
    _require_exact("contract SHA-256", manifest.contract_sha256,
                   C906L_CONTRACT_SHA256)
    _require_exact("manifest reserved1", manifest.reserved1, bytes(28))
    _require_exact("manifest commit", manifest.commit,
                   c906l_contract.MANIFEST_COMMIT)


def _stable_status_fields(status):
    return (
        status.magic,
        status.abi_major,
        status.abi_minor,
        status.struct_size,
        status.state,
        status.generation,
        status.flags,
        status.capabilities,
        status.last_request,
        status.last_response,
        status.activation_state,
        status.activation_error,
        status.activation_attempts,
        status.activation_request_id,
    )


def validate_c906l_snapshot_pair(first, second):
    """Reject torn/restarted identity and require a live forward heartbeat."""
    validate_c906l_snapshot(first)
    validate_c906l_snapshot(second)
    if first.manifest != second.manifest:
        raise C906LBringupError("C906L manifest changed between snapshots")
    if _stable_status_fields(first.status) != _stable_status_fields(second.status):
        raise C906LBringupError(
            "C906L hot status changed outside the heartbeat between snapshots"
        )
    heartbeat_delta = (second.status.heartbeat - first.status.heartbeat) & (
        (1 << 64) - 1
    )
    if heartbeat_delta == 0:
        raise C906LBringupError("C906L heartbeat did not advance")
    if heartbeat_delta >= 1 << 63:
        raise C906LBringupError("C906L heartbeat moved backwards")
    return second


def apply_c906l_reset_sequence(run_address, read_u32, write_u32):
    """Apply the vendor FSBL C906L sequence through checked callbacks.

    The callback shape keeps ordering and readback policy independently
    testable while the real runner supplies U-Boot-backed MMIO operations.
    There is intentionally no rollback reset: after the final write the core
    may own buses, so a failed readiness check requires a cold-cycle.
    """
    reset_value = read_u32(C906L_RESET_REG)
    write_u32(C906L_RESET_REG, reset_value & ~C906L_RESET_BIT)
    actual = read_u32(C906L_RESET_REG)
    if actual & C906L_RESET_BIT:
        raise C906LBringupError(
            f"C906L reset did not assert (register is 0x{actual:08x})"
        )

    sec_value = read_u32(C906L_SEC_SYS_REG)
    write_u32(C906L_SEC_SYS_REG, sec_value | C906L_SEC_ENABLE_BIT)
    sec_actual = read_u32(C906L_SEC_SYS_REG)
    if not sec_actual & C906L_SEC_ENABLE_BIT:
        raise C906LBringupError("C906L secure-system enable bit did not latch")

    write_u32(C906L_VECTOR_LOW_REG, run_address)
    write_u32(C906L_VECTOR_HIGH_REG, run_address >> 32)
    vector_low = read_u32(C906L_VECTOR_LOW_REG)
    vector_high = read_u32(C906L_VECTOR_HIGH_REG)
    if (vector_high << 32 | vector_low) != run_address:
        raise C906LBringupError(
            "C906L boot vector readback does not match run address"
        )

    reset_value = read_u32(C906L_RESET_REG)
    write_u32(C906L_RESET_REG, reset_value | C906L_RESET_BIT)
    actual = read_u32(C906L_RESET_REG)
    if not actual & C906L_RESET_BIT:
        raise C906LBringupError(
            f"C906L reset did not release (register is 0x{actual:08x})"
        )


def acquire_rom_downloader_lock():
    lock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        lock.bind(_ROM_LOCK_ADDRESS)
    except OSError as exc:
        lock.close()
        if exc.errno == errno.EADDRINUSE:
            return None
        raise
    return lock


def find_nanokvm_fastboot_serial():
    """Walk /sys/bus/usb/devices for the NanoKVM's 18d1:d00d gadget and
    return its iSerial. Returns None when not found. Returns the
    `_SERIAL_AUTO` sentinel when the gadget exists but has no
    iSerial — U-Boot's default fastboot gadget leaves it blank, so
    that's the common case. Callers should pass `_SERIAL_AUTO` through
    `fastboot_cmd` to mean ``no -s flag, let android-tools pick the
    sole connected device''.
    """
    base = "/sys/bus/usb/devices"
    if not os.path.isdir(base):
        return None
    for entry in os.listdir(base):
        try:
            with open(os.path.join(base, entry, "idVendor")) as f:
                vid = int(f.read().strip(), 16)
            with open(os.path.join(base, entry, "idProduct")) as f:
                pid = int(f.read().strip(), 16)
        except (FileNotFoundError, OSError, ValueError):
            continue
        if vid == FASTBOOT_VENDOR_ID and pid == FASTBOOT_PRODUCT_ID:
            try:
                with open(os.path.join(base, entry, "serial")) as f:
                    serial = f.read().strip()
                return serial if serial else _SERIAL_AUTO
            except (FileNotFoundError, OSError):
                return _SERIAL_AUTO
    return None


def find_nanokvm_rom_device():
    """Return True while the Cvitek BootROM USB device is enumerated."""
    base = "/sys/bus/usb/devices"
    if not os.path.isdir(base):
        return False
    for entry in os.listdir(base):
        try:
            with open(os.path.join(base, entry, "idVendor")) as f:
                vid = int(f.read().strip(), 16)
            with open(os.path.join(base, entry, "idProduct")) as f:
                pid = int(f.read().strip(), 16)
        except (FileNotFoundError, OSError, ValueError):
            continue
        if vid == ROM_VENDOR_ID and pid == ROM_PRODUCT_ID:
            return True
    return False


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument('fit', nargs='?',
                   help='path to FIT image to push and boot; optional with '
                        '--uboot-only')
    p.add_argument('--uboot-only', action='store_true',
                   help='stop after U-Boot fastboot enumerates, leaving the '
                        'board in U-Boot instead of staging/booting a FIT')
    p.add_argument('--oem-console', action='store_true',
                   help='with --uboot-only, dump U-Boot console record once '
                        'after fastboot is online')
    p.add_argument('--oem-run', action='append', default=[],
                   help='with --uboot-only, run a U-Boot command via '
                        '`fastboot oem run:<cmd>`; may be repeated')
    p.add_argument('--skip-fip', action='store_true',
                   help='skip ROM FIP push; assume U-Boot already in fastboot. '
                        'A held-reset C906L still requires an exact payload '
                        'CRC before release. An already-running core is '
                        'refused unless --accept-running-c906l is also given.')
    p.add_argument('--fip', help='path to directory containing fip.bin')
    p.add_argument('--rom-dl', help='path to cv181x-rom-dl')
    p.add_argument('--fastboot', default='fastboot',
                   help='path to fastboot binary (android-tools)')
    p.add_argument('--wait', type=float, default=40.0,
                   help='seconds to wait for fastboot gadget to enumerate')
    p.add_argument('--rom-dl-timeout', type=float, default=480.0,
                   help='total seconds to spend on rom-dl across all '
                        'attempts. The CV181x ROM only stays live ~1s per '
                        '8s cycle in USB-DL mode; one rom-dl attempt may '
                        'miss the window. Behind a USB hub the cdc_acm bind '
                        'is slower so the window is missed more often — just '
                        'retry more (see --attempts).')
    p.add_argument('--attempts', type=int, default=60,
                   help='how many rom-dl invocations to make before '
                        'giving up. Waiting for the ROM device consumes the '
                        'overall rom-dl-timeout but not an attempt transfer '
                        'window. Each detected transfer gets up to 120s (or '
                        'rom-dl-timeout / attempts when larger). After each '
                        'attempt we poll fastboot for 6s; if found, stop '
                        'retrying. Generous default so flaky hub paths still '
                        'converge.')
    p.add_argument('--rom-dl-verbose', action='store_true',
                   help='let cv181x-rom-dl write to our stderr instead of '
                        'discarding (useful for debugging which stage hung)')
    p.add_argument('--bootargs',
                   help='kernel command line; when set, sent to U-Boot via '
                        '`setenv bootargs "<str>"` before bootm. Overrides '
                        'whatever /chosen/bootargs the FIT fdt carries.')
    p.add_argument('--no-handoff-soft-disconnect',
                   dest='handoff_soft_disconnect', action='store_false',
                   default=True,
                   help='skip the SG2002 DWC2 soft-disconnect before bootm '
                        '(diagnostic A/B only; normally this prevents a '
                        'ghost USB device during Linux startup)')
    p.add_argument('--fastboot-serial',
                   help='fastboot SERIAL or device path to target. If '
                        'omitted, auto-detect from /sys/bus/usb by '
                        f'VID:PID {FASTBOOT_VENDOR_ID:04x}:{FASTBOOT_PRODUCT_ID:04x} '
                        '(U-Boot fastboot gadget). Set this if multiple '
                        'fastboot devices are co-attached or you want to '
                        'be explicit.')
    p.add_argument('--c906l-firmware',
                   help='C906L firmware file already embedded in the FIP. '
                        'Enables checked live attach or held-reset release; '
                        'all other --c906l-* layout options then become '
                        'mandatory.')
    p.add_argument('--c906l-run-address', type=parse_int,
                   help='physical DRAM address at which the FIP loaded the '
                        'C906L firmware; if supplied, must exactly match the '
                        'packaged canonical contract')
    p.add_argument('--c906l-shmem-address', type=parse_int,
                   help='physical address of the C906L status record; if '
                        'supplied, must exactly match the packaged contract')
    p.add_argument('--c906l-required-capabilities', type=parse_int,
                   help='final capability mask; if supplied, must exactly '
                        'match the packaged contract')
    p.add_argument('--c906l-cache-scratch-address', type=parse_int,
                   help='start of a safe, readable DRAM span used to evict '
                        "U-Boot's non-coherent cached status snapshot")
    p.add_argument('--c906l-cache-scratch-size', type=parse_int,
                   help='size of the cache-eviction span (at least 1 MiB)')
    p.add_argument('--c906l-ready-timeout', type=float, default=15.0,
                   help='seconds to wait for two exact, stable ABI 1.1 status '
                        'and manifest snapshots with a forward heartbeat '
                        '(default: 15)')
    p.add_argument('--c906l-poll-interval', type=float, default=0.25,
                   help='seconds between status snapshots (default: 0.25)')
    p.add_argument('--accept-running-c906l', action='store_true',
                   help='explicitly attach to an already-running C906L after '
                        'independently proving the exact FIP image. The runner '
                        'does not CRC or reset the mutable live payload, but '
                        'still requires exact contract identity, expected '
                        'activation state, and an advancing heartbeat')
    a = p.parse_args()

    if not a.uboot_only and not a.fit:
        p.error('fit is required unless --uboot-only is set')

    c906l_options = (
        a.c906l_run_address,
        a.c906l_shmem_address,
        a.c906l_required_capabilities,
        a.c906l_cache_scratch_address,
        a.c906l_cache_scratch_size,
    )
    if a.c906l_firmware is None and any(value is not None
                                        for value in c906l_options):
        p.error('--c906l-firmware is required with any other --c906l-* '
                'layout option')
    if a.c906l_firmware is not None and (
            a.c906l_cache_scratch_address is None
            or a.c906l_cache_scratch_size is None):
        p.error('--c906l-cache-scratch-address and '
                '--c906l-cache-scratch-size are all required with '
                '--c906l-firmware')
    if a.c906l_firmware is not None and not os.path.isfile(a.c906l_firmware):
        p.error(f'C906L firmware is not a regular file: {a.c906l_firmware}')
    if a.c906l_ready_timeout <= 0:
        p.error('--c906l-ready-timeout must be positive')
    if a.c906l_poll_interval <= 0:
        p.error('--c906l-poll-interval must be positive')
    canonical_options = {
        '--c906l-run-address': (
            a.c906l_run_address, c906l_contract.FIRMWARE_ADDRESS),
        '--c906l-shmem-address': (
            a.c906l_shmem_address, c906l_contract.SHMEM_ADDRESS),
        '--c906l-required-capabilities': (
            a.c906l_required_capabilities,
            c906l_contract.EXPECTED_CAPABILITIES),
    }
    for option, (actual, expected) in canonical_options.items():
        if actual is not None and actual != expected:
            p.error(
                f'{option} is 0x{actual:x}, but this runner is packaged for '
                f'0x{expected:x}'
            )
    if a.c906l_firmware is not None:
        a.c906l_run_address = c906l_contract.FIRMWARE_ADDRESS
        a.c906l_shmem_address = c906l_contract.SHMEM_ADDRESS
        a.c906l_required_capabilities = c906l_contract.EXPECTED_CAPABILITIES
    if a.accept_running_c906l and a.c906l_firmware is None:
        p.error('--accept-running-c906l requires a C906L-bearing runner')

    fit_size = os.path.getsize(a.fit) if a.fit else 0

    def log(m):
        print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)

    # Keep this object referenced until main() returns.  Closing it releases
    # the abstract socket automatically, including on SIGTERM/process death.
    rom_downloader_lock = acquire_rom_downloader_lock()
    if rom_downloader_lock is None:
        log("ERROR: another SG2002 mainline USB boot is already active on "
            "this host; refusing to race its ROM/fastboot handoff")
        sys.exit(75)

    def fastboot_cmd(*args):
        cmd = [a.fastboot]
        # `_SERIAL_AUTO` means: gadget is present but iSerial is empty
        # (U-Boot default). Skip the `-s` flag and rely on the fact
        # that we already verified exactly one fastboot device is
        # attached. A real serial gets passed through verbatim.
        if a.fastboot_serial and a.fastboot_serial != _SERIAL_AUTO:
            cmd += ['-s', a.fastboot_serial]
        return cmd + list(args)

    def run_fastboot_checked(*args, timeout=60):
        try:
            result = subprocess.run(
                fastboot_cmd(*args), capture_output=True, text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise C906LBringupError(
                f"fastboot {' '.join(args[:2])} timed out"
            ) from exc
        except OSError as exc:
            raise C906LBringupError(
                f"could not execute fastboot: {exc}"
            ) from exc
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).strip()
            raise C906LBringupError(
                f"fastboot {' '.join(args[:2])} failed: {detail}"
            )
        return result.stdout + result.stderr

    def drain_uboot_console():
        """Drain and reset U-Boot's console record before a parsed command."""
        try:
            result = subprocess.run(
                fastboot_cmd('oem', 'console'), capture_output=True,
                text=True, timeout=60,
            )
        except subprocess.TimeoutExpired as exc:
            raise C906LBringupError("fastboot oem console timed out") from exc
        except OSError as exc:
            raise C906LBringupError(
                f"could not execute fastboot oem console: {exc}"
            ) from exc
        output = result.stdout + result.stderr
        if result.returncode and 'empty console' not in output.lower():
            raise C906LBringupError(
                f"fastboot oem console failed: {output.strip()}"
            )
        return output

    def run_uboot_checked(command):
        log(f"C906L: U-Boot: {command}")
        run_fastboot_checked('oem', f'run:{command}')

    def run_uboot_with_output(command):
        drain_uboot_console()
        run_uboot_checked(command)
        output = run_fastboot_checked('oem', 'console')
        if not output.strip():
            raise C906LBringupError(
                f"U-Boot command produced no console record: {command}"
            )
        return output

    def uboot_crc32(address, size):
        output = run_uboot_with_output(
            f'crc32 0x{address:x} 0x{size:x}'
        )
        return parse_uboot_crc32(output, address, size)

    def uboot_read_words(address, count):
        output = run_uboot_with_output(
            f'md.l 0x{address:x} 0x{count:x}'
        )
        return parse_uboot_words(output, address, count)

    def uboot_write_u32(address, value):
        run_uboot_checked(f'mw.l 0x{address:x} 0x{value & 0xffffffff:08x} 1')

    def uboot_read_u32(address):
        return uboot_read_words(address, 1)[0]

    def read_c906l_snapshot():
        # U-Boot has no dcache command in this configuration and SG2002 is not
        # hardware coherent.  A full, caller-declared safe span larger than
        # the cache is read before every snapshot to evict an old status line.
        uboot_crc32(a.c906l_cache_scratch_address,
                    a.c906l_cache_scratch_size)
        words = uboot_read_words(a.c906l_shmem_address,
                                 C906L_SNAPSHOT_SIZE // 4)
        return decode_c906l_snapshot(words)

    def wait_for_c906l_ready():
        deadline = time.monotonic() + a.c906l_ready_timeout
        baseline = None
        last_error = None
        while time.monotonic() < deadline:
            snapshot = read_c906l_snapshot()
            try:
                validate_c906l_snapshot(snapshot)
            except C906LBringupError as exc:
                if baseline is not None:
                    raise C906LBringupError(
                        f"second C906L identity snapshot is invalid: {exc}"
                    ) from exc
                last_error = exc
            else:
                if baseline is None:
                    baseline = snapshot
                    log(
                        "C906L: exact ABI 1.1 identity observed: "
                        f"profile={c906l_contract.PROFILE_NAME}, "
                        f"generation={snapshot.status.generation}, "
                        f"heartbeat={snapshot.status.heartbeat}"
                    )
                else:
                    try:
                        ready = validate_c906l_snapshot_pair(
                            baseline, snapshot
                        )
                    except C906LBringupError as exc:
                        if "heartbeat did not advance" not in str(exc):
                            raise
                        last_error = exc
                        baseline = snapshot
                    else:
                        log(
                            "C906L: stable contract identity and heartbeat "
                            f"progress proven: {baseline.status.heartbeat} -> "
                            f"{ready.status.heartbeat}"
                        )
                        return ready
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(a.c906l_poll_interval, remaining))

        detail = str(last_error) if last_error else "no valid snapshot observed"
        raise C906LBringupError(
            f"C906L did not prove exact identity and a live heartbeat "
            f"within {a.c906l_ready_timeout:g}s ({detail})"
        )

    fip_transfer_attempted = False

    def bring_up_c906l():
        firmware_size = os.path.getsize(a.c906l_firmware)
        validate_c906l_layout(
            firmware_size,
            a.c906l_run_address,
            a.c906l_shmem_address,
            a.c906l_cache_scratch_address,
            a.c906l_cache_scratch_size,
        )
        reset_value = uboot_read_words(C906L_RESET_REG, 1)[0]
        mode = select_c906l_handoff_mode(
            reset_value, fip_transfer_attempted, a.accept_running_c906l
        )
        if mode != "held-reset":
            if mode == "fsbl-started":
                log("C906L: FSBL already released the core after this "
                    "runner's FIP transfer; validating live status")
            else:
                log("C906L: explicit attach to an already-running core; "
                    "payload CRC cannot be checked after firmware mutates "
                    "its data, so requiring exact contract identity, "
                    "activation state, and heartbeat progress")
            wait_for_c906l_ready()
            return
        if a.skip_fip:
            log("C906L: --skip-fip selected; held-reset and exact in-DRAM "
                "payload checks remain mandatory")
        with open(a.c906l_firmware, 'rb') as firmware:
            expected_crc = zlib.crc32(firmware.read()) & 0xffffffff
        log("C906L: verifying FIP-loaded firmware: "
            f"0x{a.c906l_run_address:x}+0x{firmware_size:x}, "
            f"CRC32 {expected_crc:08x}")
        actual_crc = uboot_crc32(a.c906l_run_address, firmware_size)
        if actual_crc != expected_crc:
            raise C906LBringupError(
                f"C906L firmware CRC32 mismatch: U-Boot read "
                f"{actual_crc:08x}, local file is {expected_crc:08x}; "
                "refusing to release the core"
            )
        log("C906L: firmware CRC32 verified")

        try:
            apply_c906l_reset_sequence(
                a.c906l_run_address, uboot_read_u32, uboot_write_u32
            )
        except BaseException:
            log("ERROR: C906L reset handoff did not complete; hardware state "
                "is ambiguous. Do not continue to Linux; cold-cycle before "
                "retrying.")
            raise

        log("C906L: reset released; waiting for firmware status")
        try:
            wait_for_c906l_ready()
        except BaseException:
            log("ERROR: C906L was released but readiness was not proven. "
                "The runner will not reset a core that may own buses; refuse "
                "Linux handoff and cold-cycle the board before retrying.")
            raise

    def find_fastboot_target():
        """Return our fastboot serial (or `_SERIAL_AUTO` for an empty-
        iSerial gadget), populating a.fastboot_serial as a side-effect
        on first detection. None when no NanoKVM gadget is attached yet
        — caller polls."""
        if a.fastboot_serial:
            return a.fastboot_serial
        serial = find_nanokvm_fastboot_serial()
        if serial:
            a.fastboot_serial = serial
            if serial == _SERIAL_AUTO:
                log(f"  NanoKVM fastboot gadget detected "
                    f"(empty iSerial — passing fastboot without -s)")
            else:
                log(f"  found NanoKVM fastboot serial: {serial}")
        return serial

    if not a.skip_fip:
        if not a.fip or not a.rom_dl:
            log("ERROR: --fip and --rom-dl are required unless --skip-fip")
            sys.exit(2)

        # cv181x-rom-dl exits non-zero (often 255) even on success when
        # the SoC ROM transitions between 1st-stage and 2nd-stage USB
        # modes — particularly under deeper hub paths. The push itself
        # may have completed; the subsequent fastboot enumeration check
        # is the source of truth, so we don't `check_call` here.
        #
        # rom-dl ALSO has a bug where after pushing FIP it enters an
        # infinite "Waiting for USB connect 2nd stage" loop polling for
        # the vendor `cvi_utask` USB gadget — which never appears with
        # mainline U-Boot. We have to kill it externally; the FIP push
        # has already happened by then. Without a timeout `subprocess`
        # would block forever.
        #
        # And — the deeper unreliability — the CV181x ROM only holds
        # the USB device live for ~1s per cycle; if pyserial's poll +
        # cdc_acm bind + open() doesn't align with that window, the FIP
        # push silently fails. We retry: each pass is one short rom-dl
        # invocation, then a fastboot probe. If fastboot enumerates,
        # we move on. If not, try rom-dl again. Stop after a.attempts.
        outsink = None if a.rom_dl_verbose else subprocess.DEVNULL
        # Per-attempt timeout floored high (120s): a *successful* push is
        # multi-stage (1st-stage FSBL → cvi_utask 2nd-stage → OpenSBI+
        # U-Boot). The Claw path through fuckup has taken about 70s in real
        # use; a 45s ceiling killed a valid push mid-2nd-stage. EIO attempts
        # (device cycled mid-send) still return quickly, so the high ceiling
        # only bites on a real push — exactly when we want to let it finish.
        #
        # rom-dl-timeout remains an overall wall-clock budget. Crucially, we
        # wait for 3346:1000 *before* starting an attempt. Previously a runner
        # armed 86s before reset could spend 86s of a 90s attempt polling and
        # then kill the newly-started transfer four seconds later.
        rom_deadline = time.monotonic() + a.rom_dl_timeout
        per_attempt = max(120.0,
                          a.rom_dl_timeout / max(a.attempts, 1))
        saw_fastboot = False
        for attempt in range(1, a.attempts + 1):
            while time.monotonic() < rom_deadline:
                seen = find_fastboot_target()
                if seen:
                    log(f"  fastboot live: {seen}")
                    saw_fastboot = True
                    break
                if find_nanokvm_rom_device():
                    break
                time.sleep(0.1)
            if saw_fastboot:
                break

            remaining = rom_deadline - time.monotonic()
            if remaining <= 0:
                log("rom-dl timeout expired while waiting for the "
                    f"{ROM_VENDOR_ID:04x}:{ROM_PRODUCT_ID:04x} ROM gadget")
                break
            attempt_timeout = min(per_attempt, remaining)
            log(f"attempt {attempt}/{a.attempts}: "
                f"rom-dl push (per-attempt timeout {attempt_timeout:.0f}s)...")
            fip_transfer_attempted = True
            try:
                rc = subprocess.call(
                    [a.rom_dl, '--image_dir', a.fip],
                    stdout=outsink,
                    stderr=outsink,
                    timeout=attempt_timeout,
                )
                log(f"  rom-dl exited {rc}")
            except subprocess.TimeoutExpired:
                log(f"  rom-dl still running at {attempt_timeout:.0f}s — killing "
                    f"(FIP push has either happened or it's stuck polling)")

            # Quick fastboot probe — if the device transitioned, exit
            # the retry loop and proceed to staging. We look directly
            # for the U-Boot fastboot gadget at 18d1:d00d so a phone
            # plugged in for unrelated reasons doesn't get treated as
            # "the device".
            probe_deadline = time.time() + 6.0
            seen = None
            while time.time() < probe_deadline:
                seen = find_fastboot_target()
                if seen:
                    break
                time.sleep(0.2)
            if seen:
                log(f"  fastboot live: {seen}")
                saw_fastboot = True
                break
            else:
                log(f"  no fastboot yet — retrying rom-dl")
        if not saw_fastboot:
            log("rom-dl never produced a fastboot gadget after "
                f"{a.attempts} attempts — giving up")

    # Wait for the fastboot gadget to appear. We look directly for our
    # VID:PID rather than trusting `fastboot devices` output, which
    # would also list any phone in bootloader mode.
    deadline = time.time() + a.wait
    target_serial = None
    while time.time() < deadline:
        target_serial = find_fastboot_target()
        if target_serial:
            log(f"fastboot device online: {target_serial}")
            break
        time.sleep(0.3)
    else:
        log("ERROR: NanoKVM fastboot gadget didn't enumerate; check the "
            "USB-C connection and that the FIP actually boots U-Boot. "
            "(Looking for VID:PID "
            f"{FASTBOOT_VENDOR_ID:04x}:{FASTBOOT_PRODUCT_ID:04x}.)")
        sys.exit(1)

    if a.c906l_firmware is not None:
        try:
            bring_up_c906l()
        except C906LBringupError as exc:
            log(f"ERROR: {exc}")
            sys.exit(1)

    if a.uboot_only:
        if a.oem_console:
            log("dumping U-Boot console record...")
            r = subprocess.run(fastboot_cmd('oem', 'console'),
                               capture_output=True, text=True)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            if r.returncode != 0:
                log(f"oem console failed with exit code {r.returncode}")

        for cmd in a.oem_run:
            log(f"issuing: oem run:{cmd}")
            r = subprocess.run(fastboot_cmd('oem', f'run:{cmd}'),
                               capture_output=True, text=True)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            if r.returncode != 0:
                log(f"oem run failed with exit code {r.returncode}")
                sys.exit(r.returncode)

        log("U-Boot fastboot is online; leaving board in U-Boot")
        return

    log(f"staging {a.fit} ({fit_size} bytes)...")
    t0 = time.time()
    r = subprocess.run(fastboot_cmd('stage', a.fit),
                       capture_output=True, text=True)
    if r.returncode != 0:
        log(f"fastboot stage FAILED: {r.stderr}")
        sys.exit(1)
    log(f"staged in {time.time()-t0:.2f}s")

    # Tell U-Boot to bootm the staged image via FASTBOOT_OEM_RUN. Note
    # the `run:<cmd>` form: U-Boot's fastboot command table uses `:` as
    # the name/param separator (strsep on cmd_string), so we must send
    # a single `oem run:<cmd>` argument, not `oem run <cmd>` — the
    # latter hits the "unrecognized command" path because "oem run
    # <cmd>" doesn't strcmp-match "oem run".
    #
    # If bootm succeeds and hands off to the kernel, U-Boot never sends
    # the OKAY response, so host-side `fastboot` returns an error /
    # times out — that's expected and harmless.
    bootargs_cmd = (
        f'setenv bootargs "{a.bootargs}"; ' if a.bootargs else ''
    )
    handoff_cmd = ''
    if a.handoff_soft_disconnect:
        handoff_cmd = (
            f'mw.l 0x{DWC2_DCTL_ADDR:08x} '
            f'0x{DWC2_DCTL_SFTDISCON:08x}; sleep 1; '
        )
    bootm_cmd = (
        f'{handoff_cmd}'
        f'{bootargs_cmd}'
        f'setenv fdt_high 0xffffffff; '
        f'setenv initrd_high 0xffffffff; '
        f'bootm 0x{FASTBOOT_BUF_ADDR:x}'
    )
    log(f"issuing: oem run:{bootm_cmd}")
    # Three normal outcomes:
    #   1. Kernel boots cleanly → U-Boot's USB gadget tears down →
    #      fastboot sees a disconnect → returncode != 0, harmless.
    #   2. bootm fails fast and returns → returncode == 0 with output —
    #      rare but useful (means the FIT is broken).
    #   3. Kernel panics → hardware watchdog resets the board → U-Boot
    #      comes back up and re-enumerates → host fastboot session is
    #      now stuck on the original `oem run`, which never gets an
    #      OKAY → subprocess timeout fires. We swallow the timeout and
    #      let the caller (mkUsbBoot wrapper) attach picocom to the
    #      ACM gadget that comes up post-reset, so the panic trace is
    #      capturable.
    try:
        r = subprocess.run(fastboot_cmd('oem', f'run:{bootm_cmd}'),
                           capture_output=True, text=True, timeout=60)
        if r.returncode == 0:
            log(f"bootm returned (unexpected — kernel may not have started):"
                f"\n{r.stdout}\n{r.stderr}")
        else:
            log("bootm handed off (fastboot disconnected — kernel is running)")
    except subprocess.TimeoutExpired:
        log("oem-run timed out — kernel likely panicked and watchdog "
            "reset the board (fastboot session never got an OKAY). "
            "Continuing so picocom can attach to the post-reset ACM gadget.")


if __name__ == '__main__':
    main()
