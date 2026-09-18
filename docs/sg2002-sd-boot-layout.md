# SG2002 SD boot layout

The persistent SD images currently use MBR, a small FAT16 firmware partition
containing `fip.bin`, and a Btrfs root. Mainline U-Boot loads the kernel,
device tree and initrd through `/boot/extlinux/extlinux.conf` on the root
partition.

The firmware partition's LBA 1 start follows the vendor image. It is not a
demonstrated requirement of the ROM.

## NanoKVM-PCIe hardware test, 2026-09-18

One SG2002 NanoKVM-PCIe was tested with the same firmware and root filesystem:

| Partition table | Firmware start | Result |
| --- | --- | --- |
| Original MBR | LBA 1 | Boots NixOS |
| MBR | LBA 2048 | Boots NixOS; Ethernet and Wi-Fi SSH verified |
| GPT with a protective MBR | LBA 2048 | Returned to USB ROM-download mode |

The aligned MBR and GPT tests used the same FAT16 filesystem, firmware file,
and partition offsets. The GPT firmware partition used the EFI System
Partition type. The root partition was neither moved nor reformatted.
Primary and backup GPT metadata passed read-back hashes and `sfdisk --verify`
before reboot.

The hardware watchdog was active during the experiment. Recovery through USB
loaded U-Boot, restored the original partition metadata and firmware area,
and compared the restored bytes before reboot. Root filesystem sectors were
not written by the layout test.

This establishes that moving the firmware partition alone works on this
board, but does **not** establish native GPT SD boot support. The result
points to an early boot partition-reader limitation; it does not identify
the exact ROM code path or establish behaviour on every SG2002 board.
Hybrid GPT/MBR layouts have not been validated. Keep the shipped layout
unchanged until an alternative is physically verified.

## GPT and UEFI are separate

GPT describes a partition table; UEFI describes firmware services and an OS
loader interface. Changing the former does not enable the latter. U-Boot
can provide [UEFI services](https://docs.u-boot.org/en/latest/develop/uefi/uefi.html),
but the SoC still needs to load U-Boot first. These images currently use
extlinux; this experiment did not test a UEFI boot path.
