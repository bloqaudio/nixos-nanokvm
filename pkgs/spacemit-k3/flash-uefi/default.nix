{ pkgs, uefiBlobs }:

pkgs.writeShellApplication {
  name = "spacemit-k3-flash-uefi";
  runtimeInputs = with pkgs; [
    coreutils
    gawk
    gnugrep
    mtdutils
  ];
  text = ''
    set -euo pipefail

    consent="''${1:-}"
    if [ "$consent" != "--yes-really-flash-spi-nor" ]; then
      echo "This flashes the K3 SPI NOR UEFI firmware partitions." >&2
      echo "It will overwrite bootinfo, fsbl, env, esos, opensbi, and uboot MTD partitions." >&2
      echo "Re-run as root with: spacemit-k3-flash-uefi --yes-really-flash-spi-nor" >&2
      exit 2
    fi

    if [ "$(id -u)" != 0 ]; then
      echo "spacemit-k3-flash-uefi must run as root." >&2
      exit 1
    fi

    if [ ! -r /proc/mtd ]; then
      echo "/proc/mtd is not readable; no MTD partition table found." >&2
      exit 1
    fi

    mtd_by_name() {
      awk -F'[:"]+' -v name="$1" '$4 == name { print "/dev/" $1 }' /proc/mtd
    }

    require_mtd() {
      name="$1"
      dev="$(mtd_by_name "$name" | head -n1)"
      if [ -z "$dev" ]; then
        echo "Missing MTD partition named '$name'." >&2
        cat /proc/mtd >&2
        exit 1
      fi
      printf '%s\n' "$dev"
    }

    bootinfo="$(require_mtd bootinfo)"
    fsbl="$(require_mtd fsbl)"
    env="$(require_mtd env)"
    esos="$(require_mtd esos)"
    opensbi="$(require_mtd opensbi)"
    uboot="$(require_mtd uboot)"

    echo "MTD partition table:"
    cat /proc/mtd
    echo
    echo "Flashing K3 UEFI SPI NOR firmware..."

    flashcp -v ${uefiBlobs}/share/spacemit-k3-uefi-flash/factory/bootinfo_spinor.bin "$bootinfo"
    flashcp -v ${uefiBlobs}/share/spacemit-k3-uefi-flash/factory/FSBL.bin "$fsbl"
    flashcp -v ${uefiBlobs}/share/spacemit-k3-uefi-flash/env.bin "$env"
    flashcp -v ${uefiBlobs}/share/spacemit-k3-uefi-flash/esos.itb "$esos"
    flashcp -v ${uefiBlobs}/share/spacemit-k3-uefi-flash/fw_dynamic.itb "$opensbi"
    flashcp -v ${uefiBlobs}/share/spacemit-k3-uefi-flash/edk2.itb "$uboot"

    sync
    echo "K3 UEFI SPI NOR flash complete. Cold power-cycle the board before relying on it."
  '';
  meta = {
    description = "Guarded SPI NOR UEFI flash helper for SpacemiT K3";
    maintainers = [ pkgs.lib.maintainers.georgewhewell ];
    platforms = [ "riscv64-linux" ];
  };
}
