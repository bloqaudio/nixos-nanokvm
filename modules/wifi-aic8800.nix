# WiFi mixin for the AIC8800 SDIO chip used on both the LicheeRV-Nano-W
# dev board and the NanoKVM-PCIe carrier. Importing this module enables
# wpa_supplicant on `wlan0` and pulls the wpa config from the flake's
# `rootWifiConf` (passed via `_module.args` so it isn't read from a
# random store path).
#
# To use:
#   imports = [ ./modules/wifi-aic8800.nix ];
# To not use: simply don't import it.
{
  config,
  lib,
  pkgs,
  rootWifiConf ? null,
  ...
}: {
  # The actual driver/firmware are pulled in by the upstream sg2002
  # module — we just opt-in here.
  sg2002.wifi = {
    enable = true;
    wpaConf = lib.mkDefault rootWifiConf;
  };

  # On mainline, WiFi needs SDIO1 wired (the WiFi-variant DTB). The
  # vendor DTS already enables it, so only override there. A carrier
  # board with its own combined DTB (PCIe = ethernet+WiFi) wins over
  # this via a higher-priority definition.
  sg2002.fdt = lib.mkIf (config.sg2002.kernel == "mainline") (
    lib.mkDefault pkgs.sg2002-dtb-mainline
  );

  assertions = [
    {
      assertion = config.sg2002.wifi.wpaConf != null;
      message = ''
        wifi-aic8800.nix was imported but no wifi.conf is present at
        the flake root. Either drop wpa_supplicant config at
        ./wifi.conf or remove the wifi mixin import.
      '';
    }
  ];
}
