{
  config,
  lib,
  pkgs,
  ...
}: let
  usbGadget = config.spacemit.k3.usbGadget;
  rescue = config.spacemit.k3.initrdRescue;
  authorizedKeys = config.spacemit.k3.authorizedKeys;
  authorizedKeysFile = pkgs.writeText "spacemit-k3-initrd-authorized-keys" (
    lib.concatStringsSep "\n" authorizedKeys + "\n"
  );
  sshdConfig = pkgs.writeText "spacemit-k3-initrd-sshd-config" ''
    Port 22
    ListenAddress 0.0.0.0
    HostKey /run/k3-initrd-sshd/ssh_host_ed25519_key
    AuthorizedKeysFile ${authorizedKeysFile}
    PermitRootLogin prohibit-password
    PasswordAuthentication no
    KbdInteractiveAuthentication no
    UsePAM no
    StrictModes no
    PidFile /run/k3-initrd-sshd/sshd.pid
  '';
  startSshd = pkgs.writeShellScript "spacemit-k3-initrd-sshd-start" ''
    set -euo pipefail

    install -d -m 0700 /run/k3-initrd-sshd
    install -d -m 0755 /run/sshd /var/empty
    if [ ! -s /run/k3-initrd-sshd/ssh_host_ed25519_key ]; then
      ${pkgs.openssh}/bin/ssh-keygen -q -t ed25519 -N "" \
        -f /run/k3-initrd-sshd/ssh_host_ed25519_key
    fi

    exec ${pkgs.openssh}/bin/sshd -D -e -f ${sshdConfig}
  '';
  setupGadget = pkgs.writeShellScript "spacemit-k3-initrd-usb-ncm-up" ''
    set -euo pipefail

    for module in roles dwc3 dwc3-of-simple configfs libcomposite u_ether usb_f_ncm; do
      modprobe "$module" 2>/dev/null || true
    done

    if ! grep -qs ' /sys/kernel/config ' /proc/mounts; then
      mount -t configfs configfs /sys/kernel/config
    fi

    udc="cad00000.usb3"
    for _ in $(seq 1 100); do
      if [ -e "/sys/class/udc/$udc" ]; then
        break
      fi

      set -- /sys/class/udc/*
      if [ -e "$1" ]; then
        udc="''${1##*/}"
        break
      fi

      sleep 0.05
    done

    if [ ! -e "/sys/class/udc/$udc" ]; then
      echo "spacemit-k3-initrd-usb-ncm: no USB device controller found" >&2
      exit 1
    fi

    g=/sys/kernel/config/usb_gadget/k3
    cfg_dir="$g/configs/c.1"
    func="$g/functions/ncm.usb0"

    mkdir -p "$g" "$cfg_dir/strings/0x409" "$g/strings/0x409" "$func"
    if current_udc="$(cat "$g/UDC" 2>/dev/null)" && [ -n "$current_udc" ]; then
      printf '%s' "" > "$g/UDC"
    fi

    echo 0x361c > "$g/idVendor"
    echo 0x0008 > "$g/idProduct"
    echo 0x0210 > "$g/bcdUSB"
    echo 0x0618 > "$g/bcdDevice"
    echo "SpacemiT" > "$g/strings/0x409/manufacturer"
    echo "K3 initrd rescue" > "$g/strings/0x409/product"
    if [ -r /proc/device-tree/serial-number ]; then
      tr -d '\000' < /proc/device-tree/serial-number > "$g/strings/0x409/serialnumber"
    else
      echo "k3-initrd-rescue" > "$g/strings/0x409/serialnumber"
    fi
    echo "NCM Configuration" > "$cfg_dir/strings/0x409/configuration"
    echo 120 > "$cfg_dir/MaxPower"
    printf '%s' "${usbGadget.deviceMac}" > "$func/dev_addr"
    printf '%s' "${usbGadget.hostMac}" > "$func/host_addr"

    [ -e "$cfg_dir/ncm.usb0" ] || ln -s "$func" "$cfg_dir/ncm.usb0"
    echo "$udc" > "$g/UDC"
  '';
  kernel = "${config.system.build.kernel}/${config.system.boot.loader.kernelFile}";
  initrd = "${config.system.build.initialRamdisk}/${config.system.boot.loader.initrdFile}";
  dtb = "${config.hardware.deviceTree.package}/${config.hardware.deviceTree.name}";
  kernelParams = lib.concatStringsSep " " config.boot.kernelParams;
  dtbWithBootargs =
    pkgs.runCommand "spacemit-k3-initrd-rescue.dtb" {
      nativeBuildInputs = [pkgs.buildPackages.dtc];
    } ''
      install -m 0644 ${dtb} "$out"
      fdtput -p -t s "$out" /chosen bootargs ${lib.escapeShellArg kernelParams}
    '';
  fitSource = pkgs.writeText "spacemit-k3-initrd-rescue.its" ''
    /dts-v1/;

    / {
      description = "SpacemiT K3 initrd rescue";
      #address-cells = <2>;

      images {
        kernel {
          description = "Linux kernel";
          data = /incbin/("${kernel}");
          type = "kernel";
          arch = "riscv";
          os = "linux";
          compression = "none";
          load = <0x00000001 0x40000000>;
          entry = <0x00000001 0x40000000>;

          hash {
            algo = "sha256";
          };
        };

        ramdisk {
          description = "NixOS initrd";
          data = /incbin/("${initrd}");
          type = "ramdisk";
          arch = "riscv";
          os = "linux";
          compression = "none";
          load = <0x00000001 0x30000000>;

          hash {
            algo = "sha256";
          };
        };

        fdt {
          description = "K3 Pico-ITX DTB";
          data = /incbin/("${dtbWithBootargs}");
          type = "flat_dt";
          arch = "riscv";
          compression = "none";
          load = <0x00000001 0x38000000>;

          hash {
            algo = "sha256";
          };
        };
      };

      configurations {
        default = "conf";

        conf {
          description = "Initrd rescue";
          kernel = "kernel";
          ramdisk = "ramdisk";
          fdt = "fdt";
        };
      };
    };
  '';
  bootImage =
    pkgs.runCommand "spacemit-k3-initrd-rescue-boot.img" {
      nativeBuildInputs = [pkgs.buildPackages.android-tools];
    } ''
      mkbootimg \
        --kernel ${config.system.build.kernel}/${config.system.boot.loader.kernelFile} \
        --ramdisk ${config.system.build.initialRamdisk}/${config.system.boot.loader.initrdFile} \
        --dtb ${config.hardware.deviceTree.package}/${config.hardware.deviceTree.name} \
        --cmdline '${lib.concatStringsSep " " config.boot.kernelParams}' \
        --header_version 2 \
        --base 0x10000000 \
        --kernel_offset 0x00008000 \
        --ramdisk_offset 0x01000000 \
        --tags_offset 0x00000100 \
        --dtb_offset 0x01f00000 \
        --pagesize 2048 \
        --output "$out"
    '';
  fitImage =
    pkgs.runCommand "spacemit-k3-initrd-rescue.fit" {
      nativeBuildInputs = [
        pkgs.buildPackages.dtc
        pkgs.buildPackages.ubootTools
      ];
    } ''
      mkimage -f ${fitSource} "$out"
    '';
in {
  imports = [
    ../boards/spacemit-k3-pico-itx.nix
    ./spacemit-k3-usb-gadget.nix
  ];

  options.spacemit.k3.initrdRescue = with lib; {
    ssh.enable = mkOption {
      type = types.bool;
      default = true;
      description = "Enable OpenSSH in the K3 initrd rescue image.";
    };

    diagnosticTools.enable = mkOption {
      type = types.bool;
      default = true;
      description = "Include disk and filesystem diagnostic tools in the K3 initrd rescue image.";
    };

    ethernetDhcp.enable = mkOption {
      type = types.bool;
      default = true;
      description = "Enable DHCP on wired Ethernet interfaces in the K3 initrd rescue image.";
    };
  };

  config = {
    assertions = [
      {
        assertion = !rescue.ssh.enable || authorizedKeys != [];
        message = "spacemit-k3-initrd-rescue requires spacemit.k3.authorizedKeys.";
      }
    ];

    networking.hostName = lib.mkDefault "k3-initrd-rescue";
    networking.firewall.enable = lib.mkDefault false;
    networking.networkmanager.enable = lib.mkDefault false;

    boot = {
      initrd = {
        availableKernelModules = [
          "btrfs"
          "dwc3"
          "dwc3-of-simple"
          "libcomposite"
          "ixgbe"
          "roles"
          "u_ether"
          "usb_f_ncm"
          "ufs-spacemit"
          "ufshcd-core"
          "ufshcd-pltfrm"
        ];
        kernelModules = [
          "btrfs"
          "dwc3"
          "dwc3-of-simple"
          "ixgbe"
          "usb_f_ncm"
        ];
        systemd = {
          root = lib.mkForce null;
          shell.enable = true;
          initrdBin =
            [
              pkgs.coreutils
              pkgs.gnugrep
              pkgs.iproute2
              pkgs.kmod
              pkgs.util-linux
            ]
            ++ lib.optionals rescue.diagnosticTools.enable [
              pkgs.bashInteractive
              pkgs.btrfs-progs
              pkgs.dosfstools
              pkgs.findutils
              pkgs.gawk
              pkgs.gnused
              pkgs.gnutar
              pkgs.gptfdisk
              pkgs.parted
              pkgs.zstd
            ];
          contents = lib.optionalAttrs rescue.ssh.enable {
            "/etc/ssh/authorized_keys.d/root".source = authorizedKeysFile;
          };
          network = {
            enable = true;
            networks."20-usb-ncm" = {
              matchConfig.MACAddress = usbGadget.deviceMac;
              address = [usbGadget.address];
              networkConfig = {
                ConfigureWithoutCarrier = true;
                IPv6AcceptRA = false;
                LinkLocalAddressing = "no";
              };
              linkConfig.RequiredForOnline = "no";
            };
            networks."30-wired-dhcp" = lib.mkIf rescue.ethernetDhcp.enable {
              matchConfig.Name = "eth* end* enp* ens*";
              networkConfig = {
                DHCP = "ipv4";
                IPv6AcceptRA = false;
                LinkLocalAddressing = "ipv4";
              };
              linkConfig.RequiredForOnline = "no";
            };
          };
          services =
            {
              k3-usb-ncm-gadget = {
                description = "SpacemiT K3 initrd USB NCM gadget";
                wantedBy = ["initrd.target"];
                before = [
                  "network-pre.target"
                  "systemd-networkd.service"
                ];
                after = [
                  "sys-kernel-config.mount"
                  "systemd-modules-load.service"
                ];
                wants = ["network-pre.target"];
                unitConfig.DefaultDependencies = false;
                serviceConfig = {
                  Type = "oneshot";
                  RemainAfterExit = true;
                  ExecStart = setupGadget;
                };
              };
            }
            // lib.optionalAttrs rescue.ssh.enable {
              k3-initrd-sshd = {
                description = "SpacemiT K3 initrd SSH";
                wantedBy = ["initrd.target"];
                after = [
                  "k3-usb-ncm-gadget.service"
                  "network.target"
                ];
                before = ["shutdown.target"];
                conflicts = ["shutdown.target"];
                unitConfig.DefaultDependencies = false;
                serviceConfig = {
                  Type = "simple";
                  Restart = "on-failure";
                  ExecStart = startSshd;
                };
              };
            };
          storePaths =
            [setupGadget]
            ++ lib.optionals rescue.ssh.enable [
              startSshd
              sshdConfig
              authorizedKeysFile
              "${pkgs.openssh}/bin/sshd"
              "${pkgs.openssh}/bin/ssh-keygen"
              "${pkgs.openssh}/libexec/sshd-auth"
              "${pkgs.openssh}/libexec/sshd-session"
            ];
        };
      };
      kernelParams = lib.mkAfter [
        "rd.systemd.unit=initrd.target"
        "boot.shell_on_fail"
      ];
      loader.grub.enable = lib.mkForce false;
      loader.systemd-boot.enable = lib.mkForce false;
      loader.generic-extlinux-compatible.enable = lib.mkForce false;
    };

    services.openssh.enable = lib.mkForce false;
    system.stateVersion = lib.mkDefault "25.05";
    system.build.spacemitK3InitrdRescueBootImage = bootImage;
    system.build.spacemitK3InitrdRescueFitImage = fitImage;
  };
}
