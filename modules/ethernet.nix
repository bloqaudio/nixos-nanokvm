# Wired ethernet for the NanoKVM-PCIe carrier: brings up `eth0` (DHCP
# via networkd). The SG2002 GMAC sits at ethernet@4070000; how it's
# driven depends on the kernel:
#
#   - vendor 5.10: the built-in `bm-dwmac` driver, enabled by the vendor
#     DTS. Load the module; the platform's vendor-gadget DTB already has
#     the node enabled.
#   - mainline: the upstream stmmac stack + `dwmac-sophgo` glue (built
#     in), bound via the PCIe DTB which flips ethernet@4070000 (and its
#     internal-EPHY mdio-mux) to `okay`. Select that DTB here.
#
# To use:  imports = [ ./modules/ethernet.nix ];
# To not:  simply don't import (e.g. the LicheeRV-Nano-W dev board).
{
  config,
  lib,
  pkgs,
  ...
}: let
  mainline = config.sg2002.kernel == "mainline";
in {
  # vendor: load bm-dwmac. mainline: the GMAC's compatible list matches
  # BOTH dwmac-sophgo (sophgo,cv1800b-dwmac) and the generic stmmac glue
  # (snps,dwmac-3.70a) — only the sophgo one sets up the internal EPHY,
  # so load it + the mdio-mux for the EPHY, and blacklist the generic
  # glue so it can't win the bind race.
  boot.kernelModules =
    lib.optionals (!mainline) ["bm-dwmac"]
    ++ lib.optionals mainline ["dwmac-sophgo" "mdio-mux-mmioreg"];
  boot.blacklistedKernelModules = lib.optionals mainline ["dwmac-generic"];

  # Mainline needs the GMAC's DT node enabled — switch the board to the
  # combined ethernet+WiFi DTB. Normal priority beats the WiFi mixin's
  # mkDefault, so a NanoKVM-PCIe that also pulls in WiFi lands on the
  # one DTB that carries both. (Vendor's DTS already enables ethernet,
  # so leave its fdt at the platform default.)
  sg2002.fdt = lib.mkIf mainline pkgs.sg2002-dtb-mainline-pcie;

  systemd.network = {
    enable = true;
    networks."20-eth0" = {
      matchConfig.Name = "eth0";
      networkConfig = {
        DHCP = "yes";
        IPv6AcceptRA = true;
      };
      linkConfig.RequiredForOnline = "no";
    };
  };
}
