# Sipeed NanoKVM-PCIe — the actual product. Carrier board adds:
#   - Wired ethernet (CV-DWMAC on ethernet@4070000)
#   - ATX header wired to gpiochip0 lines 502..505
#   - OLED footprint (may or may not be populated per unit)
#   - HDMI input on the Lattice CrossLink (cvi-vi pipeline)
#
# Same CV181x silicon as the LicheeRV-Nano-W; everything that's not
# carrier-board specific stays in `platform/cv181x.nix`.
{
  config,
  lib,
  ...
}: {
  imports = [
    ../platform/cv181x.nix
    # PCIe carrier wires RJ45 to ethernet@4070000 — pull in the
    # kernel-aware ethernet mixin (built-in vendor GMAC, stmmac +
    # the ethernet-enabled DTB on mainline).
    ../modules/ethernet.nix
    # Front-panel OLED service (top on tty1 via fbcon). Imported here —
    # not via a catalog mixin like the licheerv usb-oled target —
    # because the panel is part of the carrier board, not a build
    # variant.
    ../modules/oled.nix
    ../modules/sg2002-usb-gadget-options.nix
    ../modules/sg2002-coda.nix
  ];

  # Accurate USB gadget identity for this board (was hardcoded to the
  # LicheeRV-Nano dev board).
  sg2002.usbGadget.product = lib.mkDefault "Sipeed NanoKVM-PCIe (NixOS)";
  sg2002.usbGadget.serial = lib.mkDefault "nanokvm-pcie-0001";

  # UART1 on the carrier header has never produced usable output on the
  # physical PCIe unit. Mirror kernel logs to USB ACM from the SD profile, but
  # keep PID 1's /dev/console on the OLED-backed virtual console. A gadget TTY
  # must not become a lifetime dependency for the machine.
  sg2002.consoleDevice = "tty0";

  services.nanokvm.hardwareVersion = lib.mkDefault "pcie";
  services.nanokvm.hdmiVersion = lib.mkDefault "ux";

  # Front-panel SSD1306 128x64 OLED. The panel nodes (i2c-gpio on
  # GPIOA15/A27 + reset on A22) live in the pcie DTB itself, so unlike
  # the licheerv usb-oled target there is no variant DTB to swap in —
  # tell oled.nix to leave sg2002.fdt alone. Mainline-only: the vendor
  # DTS carries no panel node (vendor userspace bit-bangs the I2C
  # itself), so on the vendor kernel the service would just spin on a
  # missing /dev/fb0.
  #
  # Default OFF on this carrier: the fbcon -> ssd1307fb deferred-I/O
  # repaint bit-bangs the full panel over ~100 kHz GPIO I2C on the
  # single C906 every couple of seconds (procps top), which is real CPU
  # and RAM on a 256 MiB box that the capture/encode pipeline needs.
  # Opt back in with `nanokvm.oled.enable = lib.mkForce true;`.
  nanokvm.oled.enable = lib.mkDefault false;
  nanokvm.oled.useOledFdt = false;

  # The PCIe carrier labels A19/A18 as UART1, but that header is unverified.
  # Keep its getty off unless uart1Rescue is explicitly enabled for a test.
  # OpenSBI still uses UART0, so there is intentionally no UART1 earlycon.
  systemd.services."serial-getty@ttyS1" = lib.mkIf (
    config.sg2002.kernel == "mainline" && config.sg2002.uart1Rescue.enable
  ) {
    enable = true;
    wantedBy = ["multi-user.target"];
  };

  # SD card slot (sdhci0). The controller is built-in but MMC_BLOCK is a
  # module — make it available + loaded in stage-1 so /dev/mmcblk0 shows
  # up for both writing the card (live-writer initrd) and mounting root
  # from it (SD-image boot).
  sg2002.initrd.availableKernelModules = [ "mmc_block" ];
  sg2002.initrd.kernelModules = [ "mmc_block" ];

}
