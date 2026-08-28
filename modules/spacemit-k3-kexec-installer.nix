{
  config,
  lib,
  modulesPath,
  pkgs,
  ...
}:

{
  imports = [
    "${modulesPath}/installer/netboot/netboot-minimal.nix"
    ../boards/spacemit-k3-pico-itx.nix
    ./spacemit-k3-usb-gadget.nix
  ];

  options.spacemit.k3.kexecInstallerName = lib.mkOption {
    type = lib.types.str;
    default = "nixos-kexec-installer-spacemit-k3";
    description = "Name prefix for the SpacemiT K3 kexec installer tarball.";
  };

  config = {
    spacemit.k3.usbGadget.enable = true;

    boot = {
      initrd.compressor = "xz";
      loader.grub.enable = lib.mkForce false;
      loader.systemd-boot.enable = lib.mkForce false;
      postBootCommands = lib.mkForce "";
    };

    networking = {
      hostName = lib.mkDefault "k3-installer";
      firewall.enable = lib.mkDefault false;
      networkmanager.enable = lib.mkDefault false;
    };

    services.openssh.settings.PermitRootLogin = lib.mkForce "prohibit-password";
    systemd.network.wait-online.enable = lib.mkDefault false;

    system = {
      stateVersion = lib.mkDefault "25.05";
      build = {
        kexecRun = pkgs.runCommand "spacemit-k3-kexec-run"
          {
            nativeBuildInputs = [ pkgs.buildPackages.shellcheck ];
            init = "${config.system.build.toplevel}/init";
            kernelParams = lib.concatStringsSep " " config.boot.kernelParams;
          }
          ''
            substitute ${../scripts/spacemit-k3-kexec-run.sh} $out \
              --subst-var init \
              --subst-var kernelParams
            chmod 0755 $out
            shellcheck $out
          '';

        kexecInstallerTarball = pkgs.runCommand "spacemit-k3-kexec-tarball" { } ''
          mkdir kexec $out
          cp "${config.system.build.netbootRamdisk}/initrd" kexec/initrd
          cp "${config.system.build.kernel}/${config.system.boot.loader.kernelFile}" kexec/bzImage
          cp "${config.system.build.kexecRun}" kexec/run
          cp "${pkgs.pkgsStatic.kexec-tools}/bin/kexec" kexec/kexec
          cp "${pkgs.pkgsStatic.cpio}/bin/cpio" kexec/cpio
          tar -czf $out/${config.spacemit.k3.kexecInstallerName}-${pkgs.stdenv.hostPlatform.system}.tar.gz kexec
        '';
      };
    };
  };
}
