{
  config,
  lib,
  pkgs,
  ...
}: let
  kernelName = "kernel.efi";
  initrdName = "initrd";
  dtbName = "k3-pico-itx.dtb";
  kernel = "${config.system.build.kernel}/${config.system.boot.loader.kernelFile}";
  initrd = "${config.system.build.initialRamdisk}/${config.system.boot.loader.initrdFile}";
  dtb = "${config.hardware.deviceTree.package}/${config.hardware.deviceTree.name}";
  embeddedGrubConfig = pkgs.writeText "spacemit-k3-embedded-grub.cfg" ''
    set timeout=0
    set default=0

    search --no-floppy --label ESP --set=root
    configfile /EFI/nixos/grub.cfg
  '';
  writeGrubConfig = pkgs.writeShellScript "write-spacemit-k3-grub-config" ''
    set -euo pipefail

    system_path="$1"
    grub_config="$2"
    kernel_params="$(${pkgs.coreutils}/bin/tr -d '\n' < "$system_path/kernel-params")"

    ${pkgs.coreutils}/bin/cat > "$grub_config" <<EOF
    set timeout=0
    set default=0

    search --no-floppy --label ESP --set=root

    menuentry "NixOS" {
      devicetree /EFI/nixos/${dtbName}
      linux /EFI/nixos/${kernelName} init=$system_path/init $kernel_params
      initrd /EFI/nixos/${initrdName}
    }
    EOF
  '';
  grubEfi = pkgs.runCommand "spacemit-k3-bootriscv64.efi" {} ''
    ${pkgs.buildPackages.grub2}/bin/grub-mkstandalone \
      -O riscv64-efi \
      -d ${pkgs.grub2_efi}/lib/grub/riscv64-efi \
      --modules="part_gpt fat search search_label linux fdt normal configfile echo serial terminal efi_gop" \
      --locales="" \
      --fonts="" \
      -o $out \
      "boot/grub/grub.cfg=${embeddedGrubConfig}"
  '';
in {
  boot.loader = {
    grub.enable = lib.mkDefault false;
    systemd-boot.enable = lib.mkDefault false;
    generic-extlinux-compatible.enable = lib.mkDefault false;
    timeout = lib.mkDefault 3;
    efi = {
      canTouchEfiVariables = lib.mkDefault false;
      efiSysMountPoint = lib.mkDefault "/boot";
    };
    external = {
      enable = true;
      installHook = pkgs.writeShellScript "install-spacemit-k3-uefi-esp" ''
        set -euo pipefail

        system_path="$1"
        boot_dir="''${NIXOS_INSTALL_BOOT_DIR:-/boot}"

        if ! ${pkgs.util-linux}/bin/findmnt --mountpoint "$boot_dir" >/dev/null; then
          echo "refusing to install SpacemiT K3 UEFI files: $boot_dir is not mounted" >&2
          exit 1
        fi

        boot_fs="$(${pkgs.util-linux}/bin/findmnt --noheadings --output FSTYPE --target "$boot_dir")"
        if [ "$boot_fs" != vfat ]; then
          echo "refusing to install SpacemiT K3 UEFI files: $boot_dir is $boot_fs, expected vfat ESP" >&2
          exit 1
        fi

        ${pkgs.coreutils}/bin/mkdir -p "$boot_dir/EFI/BOOT" "$boot_dir/EFI/systemd" "$boot_dir/EFI/nixos"

        ${pkgs.coreutils}/bin/install -m 0644 ${grubEfi} "$boot_dir/EFI/BOOT/BOOTRISCV64.EFI"
        ${pkgs.coreutils}/bin/install -m 0644 ${kernel} "$boot_dir/EFI/nixos/${kernelName}"
        ${pkgs.coreutils}/bin/install -m 0644 ${initrd} "$boot_dir/EFI/nixos/${initrdName}"
        ${pkgs.coreutils}/bin/install -m 0644 ${dtb} "$boot_dir/EFI/nixos/${dtbName}"
        ${writeGrubConfig} "$system_path" "$boot_dir/EFI/nixos/grub.cfg"

        ${pkgs.coreutils}/bin/cat > "$boot_dir/startup.nsh" <<EOF
        @echo -off
        echo Booting NixOS...
        for %i in 0 1 2 3 4 5
          if exist FS%i:\EFI\BOOT\BOOTRISCV64.EFI then
            FS%i:\EFI\BOOT\BOOTRISCV64.EFI
          endif
        endfor
        EOF
      '';
    };
  };
}
