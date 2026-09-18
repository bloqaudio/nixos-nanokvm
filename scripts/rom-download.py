#!/usr/bin/env python3
"""Relocatable entry point to the packaged, patched vendor ROM downloader."""
from pathlib import Path
import runpy
import sys

rom = Path(__file__).resolve().parent / "rom_usb_dl"
sys.path.insert(0, str(rom))
runpy.run_path(str(rom / "cv181x_rom_usb_download.py"), run_name="__main__")
