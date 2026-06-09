# Sipeed NanoKVM-PCIe — the actual product. Carrier board adds:
#   - Wired ethernet (CV-DWMAC on ethernet@4070000)
#   - ATX header wired to gpiochip0 lines 502..505
#   - OLED footprint (may or may not be populated per unit)
#   - HDMI input on the Lattice CrossLink (cvi-vi pipeline)
#
# Same CV181x silicon as the LicheeRV-Nano-W; everything that's not
# carrier-board specific stays in `platform/cv181x.nix`.
{lib, ...}: {
  imports = [
    ../platform/cv181x.nix
    # PCIe carrier wires RJ45 to ethernet@4070000 — pull in the
    # kernel-aware ethernet mixin (bm-dwmac on vendor, stmmac + the
    # ethernet-enabled DTB on mainline).
    ../modules/ethernet.nix
  ];

  services.nanokvm.hardwareVersion = lib.mkDefault "pcie";
  services.nanokvm.hdmiVersion = lib.mkDefault "ux";

  # Accurate USB gadget identity for this board (was hardcoded to the
  # LicheeRV-Nano dev board).
  sg2002.usbGadget.product = lib.mkDefault "Sipeed NanoKVM-PCIe (NixOS)";
  sg2002.usbGadget.serial = lib.mkDefault "nanokvm-pcie-0001";

  # SD card slot (sdhci0). The controller is built-in but MMC_BLOCK is a
  # module — make it available + loaded in stage-1 so /dev/mmcblk0 shows
  # up for both writing the card (live-writer initrd) and mounting root
  # from it (SD-image boot).
  boot.initrd.availableKernelModules = [ "mmc_block" ];
  boot.initrd.kernelModules = [ "mmc_block" ];
}
