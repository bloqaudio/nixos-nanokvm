# Opt-in Bluetooth support for the AIC8800 SDIO combo radio.
#
# The radio's Bluetooth transport is not UART: FDRV creates an HCI_SDIO
# device and moves HCI packets through the same AIC firmware mailbox as
# WiFi.  This module deliberately only enables the Linux/BlueZ side; the
# SG2002 platform module selects the matching BT-enabled AIC driver build.
{
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.sg2002.bluetooth;
  oled = lib.attrByPath [ "nanokvm" "oled" ] {
    enable = false;
    useOledFdt = false;
  } config;
in {
  options.sg2002.bluetooth.enable = lib.mkEnableOption ''
    Bluetooth through the AIC8800 SDIO combo-radio mailbox
  '';

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = config.sg2002.kernel == "mainline";
        message = "sg2002.bluetooth.enable currently requires sg2002.kernel = \"mainline\".";
      }
      {
        assertion = config.sg2002.wifi.enable;
        message = "sg2002.bluetooth.enable requires the AIC8800 SDIO WiFi radio (sg2002.wifi.enable = true).";
      }
      {
        assertion = !(oled.enable && oled.useOledFdt);
        message = "sg2002.bluetooth.enable is incompatible with nanokvm.oled.useOledFdt: that DTB disables SDIO1.";
      }
    ];

    # bluetoothd is deliberately the only daemon enabled here.  The package
    # also supplies bluetoothctl, btmgmt and hciconfig for hardware bring-up.
    hardware.bluetooth = {
      enable = true;
      powerOnBoot = true;
    };
    environment.systemPackages = [ pkgs.bluez ];
  };
}
