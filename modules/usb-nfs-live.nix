# NFS-backed live root for SG2002 boards (PicoClaw bring-up). The Nix
# store is a read-only NFSv4 export from the dev host's kernel nfsd
# (trex exports /export/nix-store — a bind of /nix/store — to the LAN
# and to the USB-link CIDR; see the netboot-server profile in
# ../nixos-config), with a tmpfs overlay for writes. Completely
# stateless: / is tmpfs, nothing writable is exported, and every boot
# starts from the same pristine image (fresh ssh host keys included —
# use StrictHostKeyChecking=accept-new).
#
# The mount is performed by an initrd service, not fstab, because the
# client address is not always knowable at build time: on USB-boots it
# is the static usb0 address, on WiFi boots it is whatever DHCP hands
# wlan0. The service waits for a route to the server, derives
# clientaddr from it, and mounts ro-store + tmpfs upper + overlay.
#
# Pairs with ./usb-control.nix (kexec control socket, debug shell,
# networkd config) exactly like the NBD live module does.
{ config
, lib
, pkgs
, ...
}:
let
  protocol = import ../lib/protocol.nix;
  cfg = config.nanokvm.nfsLive;

  runRootNfs = pkgs.writeShellScript "nanokvm-run-root-nfs" ''
    set -u
    # busybox mount handles -t nfs4/-o fine (kernel-direct mount, no
    # helper needed). util-linux was pulled into the initrd for this
    # earlier; it costs ~5 MB of initrd, which matters on a 190 MB
    # board where the whole cpio unpacks into page cache at boot.
    export PATH=${lib.makeBinPath [ pkgs.busybox ]}

    server=${lib.escapeShellArg cfg.server}
    export_path=${lib.escapeShellArg cfg.storeExport}

    # Site overrides travel on the kernel cmdline (runner-appended):
    #   nanokvm.nfs_server=<ip>  nanokvm.nfs_export=<pseudo-path>
    for o in $(cat /proc/cmdline); do
      case "$o" in
        nanokvm.nfs_server=*) server="''${o#nanokvm.nfs_server=}" ;;
        nanokvm.nfs_export=*) export_path="''${o#nanokvm.nfs_export=}" ;;
      esac
    done

    echo "root-nfs: waiting for a route to $server" > /dev/kmsg
    myip=""
    for _ in $(seq 1 240); do
      out="$(ip -4 route get "$server" 2>/dev/null || true)"
      case "$out" in
        *" src "*)
          myip="$(echo "$out" | sed -n 's/.*src \([0-9.]*\).*/\1/p')"
          [ -n "$myip" ] && break
          ;;
      esac
      sleep 0.5
    done
    if [ -z "$myip" ]; then
      echo "root-nfs: no route to $server after 120s" > /dev/kmsg
      exit 1
    fi

    echo "root-nfs: mounting $server:$export_path (clientaddr $myip)" > /dev/kmsg
    mkdir -p /sysroot/nix/.ro-store /sysroot/nix/.rw-store /sysroot/nix/store
    opts="vers=4.2,addr=$server,clientaddr=$myip,hard,ro,nocto,actimeo=600"
    n=0
    until mount -t nfs4 -o "$opts" "$server:$export_path" /sysroot/nix/.ro-store; do
      n=$((n + 1))
      if [ "$n" -ge 90 ]; then
        echo "root-nfs: mount of $server:$export_path failed after 90 tries" > /dev/kmsg
        exit 1
      fi
      sleep 1
    done

    mount -t tmpfs -o mode=0755 tmpfs /sysroot/nix/.rw-store
    mkdir -p /sysroot/nix/.rw-store/store /sysroot/nix/.rw-store/work
    mount -t overlay \
      -o lowerdir=/sysroot/nix/.ro-store,upperdir=/sysroot/nix/.rw-store/store,workdir=/sysroot/nix/.rw-store/work \
      overlay /sysroot/nix/store
    echo "root-nfs: /nix/store mounted (nfs ro + tmpfs overlay)" > /dev/kmsg
  '';

  # See services.usb-rx-guard below. TX-moving/RX-stuck for two 5 s
  # windows → UDC rebind. BusyBox-only, runs in initrd and stage 2.
  rxGuard = pkgs.writeShellScript "nanokvm-usb-rx-guard" ''
    set -u
    export PATH=${lib.makeBinPath [ pkgs.busybox ]}
    G=/sys/kernel/config/usb_gadget/sg2002
    stat=/sys/class/net/usb0/statistics
    stale=0
    while :; do
      sleep 5
      [ -d "$stat" ] || continue
      rx1=$(cat "$stat/rx_packets" 2>/dev/null || echo 0)
      tx1=$(cat "$stat/tx_packets" 2>/dev/null || echo 0)
      sleep 5
      rx2=$(cat "$stat/rx_packets" 2>/dev/null || echo 0)
      tx2=$(cat "$stat/tx_packets" 2>/dev/null || echo 0)
      if [ "$rx1" = "$rx2" ] && [ "$tx1" != "$tx2" ]; then
        stale=$((stale + 1))
      else
        stale=0
      fi
      if [ "$stale" -ge 2 ]; then
        stale=0
        udc=$(cat "$G/UDC" 2>/dev/null || true)
        echo "usb-rx-guard: usb0 TX moving but RX stuck; rebinding gadget ($udc)" > /dev/kmsg
        [ -n "$udc" ] || continue
        echo "" > "$G/UDC" 2>/dev/null || true
        sleep 1
        echo "$udc" > "$G/UDC" 2>/dev/null || true
      fi
    done
  '';
in
{
  imports = [
    ./usb-control.nix
  ];

  options.nanokvm.nfsLive = with lib; {
    server = mkOption {
      type = types.str;
      default = protocol.hostIp;
      description = ''
        IP the target mounts its NFS export from. Defaults to the
        USB-ECM host address; point it at the host's LAN address for
        WiFi-booted variants. Overridable at runtime with the
        nanokvm.nfs_server= kernel cmdline arg.
      '';
    };
    storeExport = mkOption {
      type = types.str;
      default = "/nix-store";
      description = "NFSv4 pseudo-path of the read-only store export.";
    };
  };

  config = {
    nanokvm.usbControl = {
      initrd.enable = true;
      stage2.enable = true;
    };

    # / and /tmp only. The store mounts (NFS ro + tmpfs upper +
    # overlay) are done by nanokvm-root-nfs.service in the initrd —
    # clientaddr is runtime-derived there (static usb0 vs DHCP wlan0).
    fileSystems = lib.mkForce {
      "/" = {
        device = "tmpfs";
        fsType = "tmpfs";
        options = [ "mode=0755" ];
      };
      "/tmp" = {
        device = "tmpfs";
        fsType = "tmpfs";
        options = [ "mode=1777" ];
      };
    };

    # NFS and overlay are built into this kernel, and runRootNfs intentionally
    # invokes BusyBox mount. Advertising NFS through the generic NixOS helper
    # lists pulls target nfs-utils (and BIND/Kerberos/SASL) into this tiny live
    # closure without participating in the mount at all.
    boot.supportedFilesystems = lib.mkForce [ ];
    boot.initrd.supportedFilesystems = lib.mkForce [ ];

    # No NFS/overlay modules are required — but erofs/loop stay for the kexec
    # payload transport.
    sg2002.initrd.pruneKernelModules = true;
    sg2002.initrd.availableKernelModules = [
      "af_packet"
      "erofs"
      "loop"
      "overlay"
    ];

    boot.initrd.network.flushBeforeStage2 = lib.mkForce false;

    boot.initrd.systemd = {
      # The store mount itself. Runs before initrd-root-fs.target so
      # the find-nixos-closure logic sees /sysroot/nix/store; retries
      # inside the script cover slow network bring-up (DHCP on wlan0).
      services.nanokvm-root-nfs = {
        description = "Mount the NFS live root store";
        wantedBy = [ "initrd-root-fs.target" ];
        before = [ "initrd-root-fs.target" "initrd-find-etc.service" ];
        after = [
          "systemd-networkd.service"
          "usb-gadget.service"
        ];
        wants = [ "systemd-networkd.service" ];
        unitConfig.DefaultDependencies = false;
        serviceConfig = {
          Type = "oneshot";
          ExecStart = runRootNfs;
          TimeoutStartSec = "300s";
        };
      };

      # initrd-find-etc (the nixos-init /etc-overlay machinery) needs
      # the store already mounted — it resolves $toplevel/init under
      # /sysroot. It raced us once and failed the whole switch-root.
      services.initrd-find-etc = {
        after = [ "nanokvm-root-nfs.service" ];
        wants = [ "nanokvm-root-nfs.service" ];
      };

      # Keep the USB debug network alive when the initrd drops to
      # emergency mode (e.g. the store mount fails). The default
      # emergency.target isolate tears the gadget down and leaves the
      # board unreachable; with these, the debug shell on 2323 stays up
      # for post-mortem debugging.
      targets.emergency.wants = [
        "usb-gadget.service"
        "usb-debug-network.service"
        "usb-debug-shell.service"
        "usb-debug-acm-status.service"
      ];
      services = {
        usb-gadget.unitConfig.IgnoreOnIsolate = true;
        usb-debug-network.unitConfig.IgnoreOnIsolate = true;
        usb-debug-shell.unitConfig.IgnoreOnIsolate = true;
        usb-debug-acm-status.unitConfig.IgnoreOnIsolate = true;

        # dwc2 RX-stall guard. On this unit the gadget's OUT path
        # (device RX) wedges 30-90 s into boot while the IN path
        # (console, target TX) keeps working — host sees
        # `cdc_* transmit queue 0 timed out`, target usb0 RX counter
        # freezes. A UDC rebind clears it. Signature we rebind on:
        # TX moved but RX didn't, two windows in a row. Idle links
        # (no TX either) are left alone.
        usb-rx-guard = {
          description = "Rebind the USB gadget when its RX path stalls";
          wantedBy = [ "initrd.target" ];
          after = [ "usb-gadget.service" "usb-debug-network.service" ];
          wants = [ "usb-gadget.service" "usb-debug-network.service" ];
          unitConfig.DefaultDependencies = false;
          serviceConfig = {
            ExecStart = rxGuard;
            Restart = "always";
            RestartSec = "5s";
          };
        };
      };
      storePaths = [
        runRootNfs
        rxGuard
      ];
    };

    # Stage-2 instance of the RX-stall guard (the initrd one dies at
    # switch-root). Same rebind logic; NFS/TCP ride over the blip.
    systemd.services.usb-rx-guard = {
      description = "Rebind the USB gadget when its RX path stalls";
      wantedBy = [ "multi-user.target" ];
      after = [ "usb-gadget.service" ];
      wants = [ "usb-gadget.service" ];
      serviceConfig = {
        ExecStart = rxGuard;
        Restart = "always";
        RestartSec = "5s";
      };
    };
  };
}
