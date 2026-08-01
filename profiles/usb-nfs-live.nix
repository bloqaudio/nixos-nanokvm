# USB-recovery NFS-live profile. Boots the kernel+initrd uploaded over
# USB into a full NixOS stage 2 whose /nix/store is an NFSv4 export
# from the host runner (see modules/usb-nfs-live.nix) — the successor
# of usb-nbd-live.nix for day-to-day iteration, with no rootfs image
# to rebuild between generations.
#
# `nix run .#boards.picoclaw.mainline.live.usb.usb-boot` reboots the
# device, boots this system, and lands you at an SSH login on
# 10.55.0.1; detaching the debug shell kexecs the next generation.
{ config
, lib
, pkgs
, modulesPath
, rootAuthorizedKeys ? [ ]
, ...
}: {
  imports = [
    # `modulesPath` (not the nixpkgs flake input): it resolves at
    # imports-time without needing specialArgs, which keeps the whole
    # board module list re-instantiable from `_module.args.modules`.
    "${modulesPath}/profiles/image-based-appliance.nix"
    ../modules/sg2002-usb-gadget-initrd.nix
    ../modules/usb-nfs-live.nix
  ];

  networking = {
    hostName = lib.mkDefault "nanokvm-nfs-live";
    useDHCP = lib.mkForce false;
    useNetworkd = true;
    firewall.enable = lib.mkForce false;
  };

  system.nixos-init.enable = true;
  system.etc.overlay.enable = true;
  services.userborn.enable = true;

  sg2002 = {
    authorizedKeys = rootAuthorizedKeys;
    usbGadget.network.enable = true;
  };

  # Same fork-storm rationale as profiles/usb-nbd-live.nix: without
  # swap the stage-2 service startup stalls systemd's mainloop past
  # RuntimeWatchdogSec and dw_wdt resets the chip.
  zramSwap = {
    enable = true;
    algorithm = "zstd";
    memoryPercent = 50;
  };

  services.openssh = {
    enable = true;
    settings = {
      PermitRootLogin = "yes";
      PasswordAuthentication = true;
    };
  };
  systemd.services.sshd = lib.mkIf config.services.userborn.enable {
    after = [
      "systemd-tmpfiles-setup.service"
      "userborn.service"
    ];
    wants = [
      "systemd-tmpfiles-setup.service"
      "userborn.service"
    ];
  };

  users.users = {
    root = {
      initialPassword = "nixos";
      openssh.authorizedKeys.keys = rootAuthorizedKeys;
    };
    nixos = {
      isNormalUser = true;
      initialPassword = "nixos";
      extraGroups = [ "wheel" ];
      openssh.authorizedKeys.keys = rootAuthorizedKeys;
    };
  };

  nanokvm.usbControl = {
    stage2ShellUser = "nixos";
  };

  # Unlike the NBD live profile, nanokvm-server stays off here: the
  # PicoClaw work starts from a minimal base, and the server's
  # HDMI/camera expectations don't apply to this board.
  services.nanokvm.enable = lib.mkDefault false;

  # The vendor 5.10 SG2002 config lacks the
  # CONFIG_ARCH_MMAP_RND_*_MAX symbols nixpkgs' generic sysctl module
  # expects when generating this file.
  environment.etc."sysctl.d/55-nixos-aslr-entropy.conf".source = lib.mkForce (
    pkgs.writeText "empty-aslr-entropy.conf" ""
  );

  environment.defaultPackages = lib.mkForce [ ];
  documentation.enable = lib.mkForce false;
  programs.nano.enable = lib.mkForce false;
  programs.less.enable = lib.mkForce false;

  # Interactive diagnostics: btop for a richer TUI, iperf3 for USB/NFS
  # throughput checks.
  environment.systemPackages = with pkgs; [
    btop
    iperf3
    procps
  ];
}
