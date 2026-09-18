# Sipeed LicheeRV-Nano + PicoClaw expansion board. Same SG2002 silicon
# as the other boards; the expansion adds a 240x240 ST7789 SPI LCD,
# two buttons, LEDs, battery charger, and a speaker interface.  The LCD
# has a dedicated declarative artifact; the remaining peripherals are not
# brought up yet (see the PicoClaw wiki page). The main
# board carries the AIC8800 WiFi chip, so the WiFi-variant DTB and the
# wifi-aic8800 mixin apply.
#
# USB recovery uses ROM USB-DL -> FIP -> fastboot -> kernel/initrd FIT.
# Storage and any stage-2 deployment policy belong to the consuming profile.
{ lib, ... }: {
  imports = [
    ../platform/cv181x.nix
  ];

  sg2002.usbGadget = {
    product = lib.mkDefault "Sipeed LicheeRV-Nano PicoClaw (NixOS)";
    serial = lib.mkDefault "sg2002-picoclaw";
  };

  # Same rationale as boards/licheerv-nano-w.nix: keep nanokvm-server
  # from crashing on GPIO export if it ever gets enabled here.
  services.nanokvm.hardwareVersion = lib.mkDefault "pcie";
  services.nanokvm.hdmiVersion = lib.mkDefault "ux";
}
