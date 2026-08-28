{
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.spacemit.k3.usbGadget;
in {
  options.spacemit.k3.usbGadget = with lib; {
    enable = mkEnableOption "SpacemiT K3 USB NCM management gadget";

    deviceMac = mkOption {
      type = types.str;
      default = "02:86:16:70:00:01";
      description = "MAC address used by the K3 side of the USB NCM link.";
    };

    hostMac = mkOption {
      type = types.str;
      default = "02:86:16:70:00:02";
      description = "MAC address advertised for the host side of the USB NCM link.";
    };

    address = mkOption {
      type = types.str;
      default = "10.86.167.1/24";
      description = "Static address assigned to the K3 USB NCM interface.";
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.services.k3-usb-ncm-gadget = {
      description = "SpacemiT K3 USB NCM management gadget";
      wantedBy = ["multi-user.target"];
      after = ["sys-kernel-config.mount"];
      before = ["network-pre.target"];
      wants = ["network-pre.target"];
      path = [
        pkgs.coreutils
        pkgs.kmod
        pkgs.util-linux
      ];
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        ExecStart = pkgs.writeShellScript "spacemit-k3-usb-ncm-up" ''
          set -euo pipefail

          for module in dwc3 dwc3-of-simple libcomposite; do
            modprobe "$module" 2>/dev/null || true
          done

          udc="cad00000.usb3"
          for _ in $(seq 1 40); do
            if [ -e "/sys/class/udc/$udc" ]; then
              break
            fi

            set -- /sys/class/udc/*
            if [ -e "$1" ]; then
              udc="''${1##*/}"
              break
            fi

            sleep 0.25
          done

          if [ ! -e "/sys/class/udc/$udc" ]; then
            echo "No USB device controller found" >&2
            exit 1
          fi

          mountpoint -q /sys/kernel/config || mount -t configfs configfs /sys/kernel/config

          g=/sys/kernel/config/usb_gadget/g1
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
          echo "ADB device" > "$g/strings/0x409/product"
          if [ -r /proc/device-tree/serial-number ]; then
            tr -d '\000' < /proc/device-tree/serial-number > "$g/strings/0x409/serialnumber"
          else
            cat /etc/machine-id > "$g/strings/0x409/serialnumber"
          fi
          echo "NCM Configuration" > "$cfg_dir/strings/0x409/configuration"
          echo 120 > "$cfg_dir/MaxPower"
          printf '${cfg.deviceMac}' > "$func/dev_addr"
          printf '${cfg.hostMac}' > "$func/host_addr"

          [ -e "$cfg_dir/ncm.usb0" ] || ln -s "$func" "$cfg_dir/ncm.usb0"
          echo "$udc" > "$g/UDC"
        '';
        ExecStop = pkgs.writeShellScript "spacemit-k3-usb-ncm-down" ''
          set -euo pipefail

          g=/sys/kernel/config/usb_gadget/g1
          if [ -d "$g" ]; then
            echo "" > "$g/UDC" 2>/dev/null || true
            rm -f "$g/configs/c.1/ncm.usb0" || true
          fi
        '';
      };
    };

    systemd.network.networks."20-usb-ncm" = {
      matchConfig.MACAddress = cfg.deviceMac;
      address = [cfg.address];
      networkConfig = {
        DHCP = "no";
        IPv6AcceptRA = false;
        LinkLocalAddressing = "no";
        IgnoreCarrierLoss = true;
      };
      linkConfig.RequiredForOnline = "no";
    };
  };
}
