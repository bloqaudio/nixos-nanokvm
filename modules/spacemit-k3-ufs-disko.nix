{pkgs, ...}: let
  imageBuilderPkgs = pkgs.buildPackages.extend (final: prev: {
    aggregateModules = modules:
      (prev.aggregateModules modules).overrideAttrs (old: {
        passthru =
          (old.passthru or {})
          // {
            target = (builtins.head modules).target;
          };
      });
  });
in {
  disko = {
    imageBuilder = {
      enableBinfmt = true;
      imageFormat = "raw";
      pkgs = imageBuilderPkgs;
      kernelPackages = imageBuilderPkgs.linuxPackages_latest;
    };

    devices.disk.sda = {
      type = "disk";
      device = "/dev/sda";
      imageName = "spacemit-k3-pico-itx-ufs";
      imageSize = "16G";
      content = {
        type = "gpt";
        partitions = {
          ESP = {
            priority = 1;
            # K3 U-Boot's UFS boot path looks up this GPT name as "ESP".
            label = "ESP";
            size = "256M";
            type = "EF00";
            content = {
              type = "filesystem";
              format = "vfat";
              extraArgs = ["-n" "ESP"];
              mountpoint = "/boot";
              mountOptions = ["umask=0077"];
            };
          };
          rootfs = {
            priority = 2;
            label = "nixos-rootfs";
            size = "100%";
            content = {
              type = "filesystem";
              format = "btrfs";
              extraArgs = [
                "-f"
                "-L"
                "rootfs"
              ];
              mountpoint = "/";
              mountOptions = [
                "compress=zstd"
                "noatime"
              ];
            };
          };
        };
      };
    };
  };
}
