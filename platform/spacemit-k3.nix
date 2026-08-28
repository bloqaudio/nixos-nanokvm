{
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.spacemit.k3;
in {
  options.spacemit.k3 = with lib; {
    enable = mkEnableOption "SpacemiT K3 board support";

    authorizedKeys = mkOption {
      type = types.listOf types.str;
      default = [];
      description = "SSH public keys baked into root's authorized_keys.";
    };

    serialConsole.enable = mkOption {
      type = types.bool;
      default = true;
      description = "Whether UART0 is used as the kernel console and serial getty.";
    };
  };

  config = lib.mkIf cfg.enable (lib.mkMerge [
    {
      assertions = [
        {
          assertion = pkgs ? "spacemit-k3-linux";
          message = ''
            SpacemiT K3 support requires this flake's overlay. Import
            nixosModules.overlay or a board module under nixosModules.boards.k3.
          '';
        }
      ];

      hardware.enableAllHardware = lib.mkForce false;
      hardware.firmware = [pkgs."spacemit-k3-rtw89-firmware"];
      hardware.deviceTree = {
        enable = lib.mkDefault true;
        name = lib.mkDefault "spacemit/k3-pico-itx.dtb";
      };

      boot = {
        consoleLogLevel = lib.mkDefault 7;
        kernelPackages = lib.mkDefault pkgs."linuxPackages_spacemit-k3";
        kernelParams =
          lib.optionals cfg.serialConsole.enable [
            "earlycon=sbi"
            "keep_bootcon"
            "console=ttyS0,115200"
            "systemd.journald.forward_to_console=1"
          ]
          ++ [
            "clk_ignore_unused"
            "pd_ignore_unused"
            "rootwait"
            "boot_mode=nor"
            "random.trust_bootloader=1"
            "systemd.log_target=kmsg"
            "systemd.log_level=info"
          ]
          ++ lib.optionals (! cfg.serialConsole.enable) [
            "console=tty0"
          ];
        initrd = {
          availableKernelModules = [
            "usb_storage"
            "uas"
            "nvme"
            "nvme_core"
          ];
          kernelModules = [
            "btrfs"
            "erofs"
            "loop"
          ];
          systemd.enable = lib.mkDefault true;
          systemd.services.k3-ufs-rebind = {
            description = "Rebind SpacemiT K3 UFS controller if first probe misses the disk";
            wantedBy = ["initrd.target"];
            before = ["sysroot.mount"];
            after = ["systemd-udev-settle.service"];
            unitConfig.DefaultDependencies = false;
            serviceConfig = {
              Type = "oneshot";
              RemainAfterExit = true;
            };
            script = ''
              if [ -e /dev/disk/by-label/rootfs ] || [ -e /dev/disk/by-partlabel/nixos-rootfs ]; then
                exit 0
              fi

              if [ ! -e /sys/bus/platform/devices/c0e00000.ufshc/driver ]; then
                echo c0e00000.ufshc > /sys/bus/platform/drivers/ufshcd-spacemit/bind 2>/dev/null || true
              fi

              i=0
              while [ "$i" -lt 40 ]; do
                if [ -e /dev/disk/by-label/rootfs ] || [ -e /dev/disk/by-partlabel/nixos-rootfs ]; then
                  break
                fi
                sleep 0.25
                i=$((i + 1))
              done
            '';
          };
        };
        supportedFilesystems.btrfs = lib.mkDefault true;
        supportedFilesystems.zfs = lib.mkForce false;
        zfs.forceImportRoot = lib.mkDefault false;
      };

      networking = {
        useNetworkd = lib.mkDefault true;
        useDHCP = lib.mkDefault false;
        firewall.enable = lib.mkDefault false;
      };

      system.nixos-init.enable = lib.mkDefault true;
      system.etc.overlay.enable = lib.mkDefault true;
      services.userborn.enable = lib.mkDefault true;

      systemd.suppressedSystemUnits = lib.optionals (! cfg.serialConsole.enable) [
        "serial-getty@ttyS0.service"
      ];

      systemd.services.systemd-reboot = {
        unitConfig.SuccessAction = lib.mkForce "none";
        serviceConfig = {
          Type = "oneshot";
          ExecStart = lib.mkForce "${pkgs.util-linux}/bin/reboot -ff";
        };
      };
    }

    (lib.mkIf (cfg.authorizedKeys != []) {
      users.users.root.openssh.authorizedKeys.keys = cfg.authorizedKeys;
    })

    (lib.mkIf cfg.serialConsole.enable {
      systemd.services."serial-getty@ttyS0" = {
        enable = true;
        wantedBy = ["getty.target"];
        serviceConfig.Restart = "always";
      };
    })
  ]);
}
