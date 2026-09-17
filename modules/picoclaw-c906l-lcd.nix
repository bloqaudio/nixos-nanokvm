# Hardware composition shared by the reversible live test and the persistent
# SD image for the PicoClaw C906L-owned LCD.  Keep this separate from boot
# transport and authentication policy: all consumers must use the exact same
# firmware, contract and DT, but they need not share a root filesystem.
{
  config,
  lib,
  pkgs,
  ...
}: {
  sg2002 = {
    auxCore = {
      enable = true;
      peripherals = pkgs.sg2002-c906l-profile-manifest.picoclaw-lcd.peripherals;
      fdt = pkgs.sg2002-dtb-mainline-picoclaw-c906l-lcd-for
        (pkgs.sg2002-c906l-contract-for-profile "picoclaw-lcd");
    };

    # SDIO remains Linux-owned; the C906L-backed regulator mediates GPIOA26
    # through the LCD task's single owner of the complete GPIOA bank.
    wifi.enable = true;
    usbGadget.console.enable = false;
  };

  # This DT deliberately removes the NanoKVM carrier peripherals.  Do not
  # start userspace that may assume they are present.
  services.nanokvm.enable = lib.mkForce false;

  environment.systemPackages = [
    pkgs.sg2002-c906l-drm-test
    (pkgs.sg2002-c906l-ctl-for
      (pkgs.sg2002-c906l-contract-for config.sg2002.auxCore.peripherals))
  ];
}
