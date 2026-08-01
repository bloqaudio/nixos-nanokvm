#!/usr/bin/env python3
import argparse
import pathlib
import sys
import time

import usb.core
import usb.util


VID = 0x361C
PID = 0x1001
EP_IN = 0x81
EP_OUT = 0x02


def find_device(timeout_s):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is not None:
            return dev
        time.sleep(0.1)
    raise RuntimeError("SpacemiT K3 fastboot USB device not found")


def open_device(timeout_s):
    dev = find_device(timeout_s)
    dev.set_configuration()
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass
    usb.util.claim_interface(dev, 0)
    return dev


def read_response(dev, timeout_ms):
    data = bytes(dev.read(EP_IN, 64, timeout=timeout_ms))
    if len(data) < 4:
        raise RuntimeError(f"short fastboot response: {data!r}")
    code = data[:4].decode("ascii", "replace")
    msg = data[4:].decode("ascii", "replace").rstrip("\x00")
    return code, msg


def send_command(dev, command, timeout_ms, allow_no_response=False):
    dev.write(EP_OUT, command.encode("ascii"), timeout=timeout_ms)
    try:
        return read_response(dev, timeout_ms)
    except usb.core.USBError:
        if allow_no_response:
            return None, "no response"
        raise


def download(dev, payload, timeout_ms):
    size = len(payload)
    code, msg = send_command(dev, f"download:{size:08x}", timeout_ms)
    if code != "DATA":
        raise RuntimeError(f"download rejected: {code}{msg}")

    expected = int(msg[:8], 16)
    if expected != size:
        raise RuntimeError(f"device requested {expected} bytes, expected {size}")

    chunk = 1024 * 1024
    sent = 0
    view = memoryview(payload)
    while sent < size:
        end = min(sent + chunk, size)
        dev.write(EP_OUT, view[sent:end], timeout=timeout_ms)
        sent = end
        if sent == size or sent % (8 * 1024 * 1024) == 0:
            print(f"sent {sent}/{size}", file=sys.stderr, flush=True)

    code, msg = read_response(dev, timeout_ms)
    if code != "OKAY":
        raise RuntimeError(f"download failed after transfer: {code}{msg}")


def main():
    parser = argparse.ArgumentParser(
        description="Send raw fastboot commands to SpacemiT K3 U-Boot."
    )
    parser.add_argument("image", nargs="?", type=pathlib.Path)
    parser.add_argument("--wait", type=float, default=20.0)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument(
        "--ucmd",
        action="append",
        default=[],
        help=(
            "Run a raw U-Boot fastboot UCmd after download. May be repeated. "
            "If present, bare fastboot boot is not sent."
        ),
    )
    parser.add_argument(
        "--allow-final-no-response",
        action="store_true",
        help="Treat a missing response from the final UCmd as success, for commands that reset or boot.",
    )
    args = parser.parse_args()

    if args.image is None and not args.ucmd:
        parser.error("either an image or at least one --ucmd is required")

    payload = args.image.read_bytes() if args.image is not None else None
    if payload is not None:
        print(f"raw fastboot download: {args.image} ({len(payload)} bytes)", file=sys.stderr)

    dev = open_device(args.wait)
    try:
        if payload is not None:
            download(dev, payload, args.timeout_ms)
        if args.ucmd:
            for index, command in enumerate(args.ucmd):
                print(f"sending UCmd: {command}", file=sys.stderr)
                last = index == len(args.ucmd) - 1
                code, msg = send_command(
                    dev,
                    f"UCmd:{command}",
                    args.timeout_ms,
                    allow_no_response=last and args.allow_final_no_response,
                )
                if code is None:
                    print("no final response; assuming USB handoff/reset", file=sys.stderr)
                    break
                print(f"{code}{msg}", file=sys.stderr)
                if code != "OKAY":
                    raise RuntimeError(f"UCmd rejected: {code}{msg}")
        else:
            print("download complete; sending bare boot", file=sys.stderr)
            code, msg = send_command(dev, "boot", args.timeout_ms)
            print(f"{code}{msg}", file=sys.stderr)
            if code != "OKAY":
                raise RuntimeError(f"boot rejected: {code}{msg}")
    finally:
        try:
            usb.util.release_interface(dev, 0)
        except usb.core.USBError:
            pass


if __name__ == "__main__":
    main()
