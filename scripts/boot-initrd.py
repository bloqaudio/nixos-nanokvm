#!/usr/bin/env python3
"""Upload the accompanying RAM-only image; needs Python, pyserial and fastboot."""
import os
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent
    if sys.platform != "linux":
        sys.exit("The ROM uploader currently requires Linux (native or a USB-passthrough VM). See README.md.")
    env = os.environ.copy()
    env["PYTHONPATH"] = str(root / "contract") + os.pathsep + env.get("PYTHONPATH", "")
    args = [
        sys.executable, str(root / "usb_boot_mainline.py"),
        str(root / "boot.itb"), "--fip", str(root / "fip"),
        "--rom-dl", str(root / "rom-download.py"), "--uboot-watchdog",
        "--bootargs", (root / "bootargs").read_text().strip(),
    ]
    if (root / "c906l.bin").exists():
        args += ["--c906l-firmware", str(root / "c906l.bin"),
                 "--c906l-cache-scratch-address", "0x82000000",
                 "--c906l-cache-scratch-size", "0x100000",
                 "--c906l-ready-timeout", "15"]
    return subprocess.call(args + sys.argv[1:], env=env)


if __name__ == "__main__":
    sys.exit(main())
