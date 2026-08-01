# SG2002 SD-card boot media, defined with disko.
#
# The SG2002 ROM is picky: the FAT firmware partition containing fip.bin
# must start at LBA 1. Use disko's legacy MSDOS table backend so the disk
# layout itself is declarative instead of patching a generic sd-image after
# the fact.
{
  config,
  lib,
  pkgs,
  ...
}: let
  firmwareLabel = "FIRMWARE";
  rootLabel = "NIXOS_SD";
  imageName = "${config.networking.hostName}-sg2002-sd";
  partitionDevice = device: index:
    if builtins.match "/dev/(disk|zvol)/.+" device != null
    then "${device}-part${toString index}"
    else if builtins.match "/dev/((nvme|mmcblk).+|md/.*[[:digit:]])" device != null
    then "${device}p${toString index}"
    else "${device}${toString index}";
  nativePkgs = import pkgs.path {
    system = pkgs.buildPackages.stdenv.hostPlatform.system;
    config = pkgs.config;
  };
  imageBuilderPkgs = nativePkgs.extend (final: prev: {
    aggregateModules = modules:
      (prev.aggregateModules modules).overrideAttrs (old: {
        passthru =
          (old.passthru or {})
          // {
            target = (builtins.head modules).target;
          };
      });
  });
  diskDevice = config.disko.devices.disk.sg2002-sd.device;
  firmwarePart = partitionDevice diskDevice 1;
  rootPart = partitionDevice diskDevice 2;
  targetExtlinuxBuilder = import "${pkgs.path}/nixos/modules/system/boot/loader/generic-extlinux-compatible/extlinux-conf-builder.nix" {
    inherit lib pkgs;
  };
  targetExtlinuxBuilderArgs =
    "-g ${toString config.boot.loader.generic-extlinux-compatible.configurationLimit} "
    + "-t ${if config.boot.loader.timeout == null then "-1" else toString config.boot.loader.timeout}"
    + lib.optionalString (config.hardware.deviceTree.name != null) " -n ${config.hardware.deviceTree.name}"
    + lib.optionalString (!config.boot.loader.generic-extlinux-compatible.useGenerationDeviceTree) " -r";
  formatSg2002Filesystems = pkgs.writeShellApplication {
    name = "format-sg2002-sd-filesystems";
    text = ''
      set -euo pipefail

      for part in "${firmwarePart}" "${rootPart}"; do
        for attempt in $(${imageBuilderPkgs.coreutils}/bin/seq 1 120); do
          if [ -b "$part" ]; then
            break
          fi
          if [ "$attempt" -eq 120 ]; then
            echo "partition $part did not appear" >&2
            exit 1
          fi
          ${imageBuilderPkgs.coreutils}/bin/sleep 1
        done
      done

      ${imageBuilderPkgs.dosfstools}/bin/mkfs.vfat -F 32 -n ${firmwareLabel} "${firmwarePart}"
      ${imageBuilderPkgs.e2fsprogs}/bin/mkfs.ext4 -F -L ${rootLabel} "${rootPart}"

      ${imageBuilderPkgs.systemdMinimal}/bin/udevadm trigger --subsystem-match=block || true
      ${imageBuilderPkgs.systemdMinimal}/bin/udevadm settle --timeout=120 || true
    '';
  };
  formatMount = pkgs.writeShellApplication {
    name = "sg2002-disko-format-mount";
    text = ''
      set -euo pipefail
      ${lib.getExe config.system.build.format}
      ${lib.getExe formatSg2002Filesystems}
      ${lib.getExe config.system.build.mount}
    '';
  };
  destroyFormatMount = pkgs.writeShellApplication {
    name = "sg2002-disko-destroy-format-mount";
    text = ''
      set -euo pipefail
      ${lib.getExe config.system.build.destroy} "$@"
      ${lib.getExe config.system.build.format}
      ${lib.getExe formatSg2002Filesystems}
      ${lib.getExe config.system.build.mount}
    '';
  };
  installBootLoader = pkgs.writeShellScript "install-sg2002-extlinux-boot" ''
    set -euo pipefail

    ${pkgs.coreutils}/bin/mkdir -p /boot
    ${targetExtlinuxBuilder} ${targetExtlinuxBuilderArgs} -c "$@" -d /boot

    if [ -d /firmware ]; then
      ${pkgs.coreutils}/bin/install -D -m 0644 ${config.system.build.fip}/fip.bin /firmware/fip.bin
      ${pkgs.coreutils}/bin/sync -f /firmware/fip.bin 2>/dev/null || ${pkgs.coreutils}/bin/sync
    else
      echo "warning: /firmware is not mounted; fip.bin was not installed" >&2
    fi
  '';
  sdImage = pkgs.runCommand "${imageName}-${config.system.nixos.label}" {} ''
    mkdir -p "$out/sd-image" "$out/nix-support"
    ln -s ${config.system.build.diskoImages}/${imageName}.raw \
      "$out/sd-image/${imageName}-${config.system.nixos.label}-${pkgs.stdenv.hostPlatform.system}.img"
    echo "file sd-image $out/sd-image/${imageName}-${config.system.nixos.label}-${pkgs.stdenv.hostPlatform.system}.img" \
      > "$out/nix-support/hydra-build-products"
    echo "${pkgs.stdenv.hostPlatform.system}" > "$out/nix-support/system"
  '';
in {
  config = {
    # The SG2002 does not need nixpkgs' generic SD-card initrd module set.
    sg2002.initrd.pruneKernelModules = true;

    boot = {
      growPartition = lib.mkDefault true;
      supportedFilesystems = lib.mkDefault [
        "ext4"
        "vfat"
      ];
      initrd.supportedFilesystems = lib.mkDefault [
        "ext4"
      ];
      kernelParams = [
        "root=/dev/disk/by-label/${rootLabel}"
        "rootwait"
        "rw"
        "rootfstype=ext4"
        "console=ttyS0,115200"
        "earlycon=sbi"
        "ignore_loglevel"
      ];
    };

    disko = {
      # The legacy `table` backend is required here because the SG2002 ROM
      # expects a FAT partition starting at LBA 1. It can create/format/mount
      # that layout, but its generated NixOS fileSystems config is not reliable
      # for this table type, so fileSystems are declared explicitly below.
      enableConfig = false;

      imageBuilder = {
        enableBinfmt = true;
        imageFormat = "raw";
        name = imageName;
        copyNixStoreThreads = 4;
        pkgs = imageBuilderPkgs;
        kernelPackages = imageBuilderPkgs.linuxPackages_latest;
      };

      devices.disk.sg2002-sd = {
        type = "disk";
        device = "/dev/mmcblk0";
        imageName = imageName;
        imageSize = lib.mkDefault "4G";
        content = {
          type = "table";
          format = "msdos";
          partitions = [
            {
              name = "firmware";
              part-type = "primary";
              fs-type = "fat32";
              start = "1s";
              end = "32768s";
              bootable = true;
            }
            {
              name = "root";
              part-type = "primary";
              fs-type = "ext4";
              start = "49152s";
              end = "100%";
            }
          ];
        };
      };

      devices.nodev = {
        "/" = {
          fsType = "ext4";
          device = "/dev/disk/by-label/${rootLabel}";
          mountOptions = [
            "noatime"
          ];
        };
        "/firmware" = {
          fsType = "vfat";
          device = "/dev/disk/by-label/${firmwareLabel}";
          mountOptions = [
            "umask=0022"
          ];
        };
      };
    };

    fileSystems = {
      "/" = {
        device = "/dev/disk/by-label/${rootLabel}";
        fsType = "ext4";
        options = [
          "noatime"
        ];
        neededForBoot = true;
        autoResize = true;
      };
      "/firmware" = {
        device = "/dev/disk/by-label/${firmwareLabel}";
        fsType = "vfat";
        options = [
          "umask=0022"
          "nofail"
        ];
        neededForBoot = true;
      };
    };

    system.build = {
      formatMount = lib.mkForce formatMount;
      destroyFormatMount = lib.mkForce destroyFormatMount;
      installBootLoader = lib.mkForce installBootLoader;
      sdImage = sdImage;
      sdImageRaw = config.system.build.diskoImages;
      sdImageScript = config.system.build.diskoImagesScript;
    };
    system.boot.loader.id = lib.mkForce "sg2002-extlinux";
  };
}
