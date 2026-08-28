# SG2002 Coda980 H.264 encoder userspace/firmware contract. The device-tree
# node remains board/variant specific; importing this module makes an enabled
# node usable once Linux reaches stage 2.
{
  config,
  lib,
  pkgs,
  ...
}: {
  config = lib.mkIf (config.sg2002.kernel == "mainline") {
    # Preserve the literal name consumed by request_firmware(). NixOS may
    # compress the source firmware in the system closure, while the standard
    # /run/current-system/firmware link gives the module loader its canonical
    # search root.
    hardware.firmware = [ pkgs.sg2002-coda980-firmware ];
    environment.systemPackages = [ pkgs.sg2002-h264-bridge ];
    boot.kernelModules = [ "coda-vpu" ];
    systemd.tmpfiles.rules = [
      "L+ /lib/firmware - - - - /run/current-system/firmware"
    ];
  };
}
