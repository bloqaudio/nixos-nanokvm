# Catalog of board × kernel × profile × variant combinations.
#
# One record per shipped configuration. lib/catalog.nix is the single
# source of truth for what flat `nixosConfigurations.<...>` names and
# `legacyPackages.<sys>.boards.<...>` expose; flake.nix doesn't repeat the
# matrix on the artifact side anymore.
#
# Record schema:
#   path         — list-of-string attrpath in `boards`:
#                  ["licheerv" "mainline" "live" "usb-rndis"]
#   boardName    — the module file under ./boards/ (e.g. "licheerv-nano-w")
#   kernel       — "mainline" | "vendor"
#   profile      — module name under ./profiles/ (no ".nix" suffix)
#   variant      — short variant name, used for DTB selection on the
#                  artifact side. Null when not applicable.
#   tag          — payload/runner identifier baked into filenames and
#                  the `nanokvm.kexec_target=` cmdline arg. Keep stable
#                  across refactors so on-device diagnostics don't drift.
#   mixins       — extra module file paths
#   modules      — extra inline module functions
#   artifact     — "kernel-test" | "live" | "debug" | "sd"
#                  drives which artifact-builder runs.
#   artifactArgs — key/value extras forwarded to the artifact builder
#                  (oled, rootfsBindIp, requireRootfsHostOverride,
#                  extraBootargs, includeKexec).
#   liveCfgPath  — `debug` artifacts only: the catalog path that
#                  supplies the rootfs the debug payload pivots into.
#
# Anything not listed here is intentionally absent — vendor variant
# transports, vendor.kexec runners, etc. — those don't work, so they
# don't exist.
{ lib }:
let
  lichee =
    kernel: pathTail: attrs:
    {
      path = [ "licheerv" kernel ] ++ pathTail;
      boardName = "licheerv-nano-w";
      inherit kernel;
    }
    // attrs;

  licheeProfile =
    kernel: pathTail: profile: artifact: tag: attrs:
    lichee kernel pathTail (
      {
        inherit profile artifact tag;
      }
      // attrs
    );

  kernelTest = kernel:
    licheeProfile
      kernel
      [ "kernel-test" ]
      "usb-kernel-test"
      "kernel-test"
      "kernel-test-${kernel}"
      { };

  debug = kernel:
    licheeProfile
      kernel
      [ "debug" ]
      "usb-debug"
      "debug"
      "debug-${kernel}"
      {
        liveCfgPath = [ "licheerv" kernel "live" "usb" ];
      };

  live =
    kernel: leaf: tag: attrs:
    licheeProfile
      kernel
      [ "live" leaf ]
      "usb-nbd-live"
      "live"
      tag
      attrs;

  usbTransport = transport: {
    modules = [ ({ ... }: { sg2002.usbGadget.network.transport = transport; }) ];
  };

  gMulti = {
    modules = [
      ({ lib, ... }: {
        boot.initrd.systemd.services.usb-gadget.enable = lib.mkForce false;
      })
    ];
    artifactArgs.extraBootargs = [
      "g_multi.use_rndis=1"
      # MACs match sg2002-usb-gadget-initrd.nix → protocol.nix.
      "g_multi.dev_addr=02:1a:11:00:01:01"
      "g_multi.host_addr=02:1a:11:00:01:02"
      "g_multi.removable=1"
      "g_multi.iManufacturer=Sipeed"
      "g_multi.iProduct=LicheeRV-Nano-NixOS"
      "g_multi.iSerialNumber=sg2002-g-multi"
    ];
  };

  oled = {
    mixins = [ ../modules/oled.nix ];
    modules = [ ({ ... }: { nanokvm.oled.enable = true; }) ];
    artifactArgs.oled = true;
  };

  wifi = {
    variant = "wifi";
    mixins = [
      ../modules/sg2002-initrd-wifi.nix
      ../modules/wifi-aic8800.nix
    ];
    modules = [
      ({ ... }: {
        # In wifi mode the rootfs NBD lives on the caller's LAN:
        # networkd brings wlan0 up via DHCP and USB-ECM stays up only
        # for the control plane. The runner must provide
        # NANOKVM_NBD_ROOTFS_HOST so this catalog remains site-neutral.
        nanokvm.nbdLive = {
          staticIface = null;
        };
      })
    ];
    artifactArgs.requireRootfsHostOverride = true;
  };

  vendorUsb = {
    # Vendor 5.10 lacks kexec-tools/nbd-client + our nbd patch — drop
    # the kexec runner from the output set. usb-boot still publishes.
    artifactArgs.includeKexec = false;
  };

  # nanokvm-pcie carrier (ethernet + WiFi + OLED footprint), mirroring
  # the `lichee` helpers above so PCIe entries stay one-liners too.
  pcie =
    kernel: pathTail: attrs:
    {
      path = [ "pcie" kernel ] ++ pathTail;
      boardName = "nanokvm-pcie";
      inherit kernel;
    }
    // attrs;

  pcieLive =
    kernel: tag: attrs:
    pcie kernel [ "live" "usb" ] (
      {
        profile = "usb-nbd-live";
        artifact = "live";
        inherit tag;
      }
      // attrs
    );

  pcieKernelTest = kernel:
    pcie kernel [ "kernel-test" ] {
      profile = "usb-kernel-test";
      artifact = "kernel-test";
      tag = "kernel-test-pcie-${kernel}";
    };

  # PCIe-live bring-up extras:
  #   - WiFi driver only, so wlan0 enumerates and the radio is
  #     exercisable. Association remains downstream policy.
  #   - nanokvm-server (the web UI + ATX/GPIO control), which the live
  #     profile doesn't enable on its own.
  pcieLiveExtras = {
    modules = [
      ({ ... }: {
        sg2002.wifi.enable = true;
        services.nanokvm = {
          enable = true;
          openFirewall = true;
        };
      })
    ];
  };

  # LicheeRV-Nano PicoClaw (SG2002 + expansion board, no SD slot in
  # use). USB-boot only; the live profile is NFS-rooted, not NBD.
  picoclaw =
    kernel: pathTail: attrs:
    {
      path = [ "picoclaw" kernel ] ++ pathTail;
      boardName = "licheerv-nano-picoclaw";
      inherit kernel;
    }
    // attrs;

  picoclawKernelTest = kernel:
    picoclaw kernel [ "kernel-test" ] {
      profile = "usb-kernel-test";
      artifact = "kernel-test";
      tag = "kernel-test-picoclaw-${kernel}";
      # The dwc2 gadget (net function AND ACM console) dies ~30-60 s
      # into every boot, exactly when the system goes idle after
      # bring-up. Suspect: C906 WFI cpuidle gating something the USB
      # controller needs. cpuidle.off=1 is the A/B test.
      artifactArgs.extraBootargs = [ "cpuidle.off=1" ];
    };

  picoclawLive = kernel: tag: attrs:
    picoclaw kernel [ "live" "usb" ] (
      {
        profile = "usb-nfs-live";
        artifact = "nfs-live";
        inherit tag;
        modules = [ ({ ... }: { sg2002.usbGadget.network.transport = "ncm"; }) ];
      }
      // attrs
    );
in
[
  # ===== licheerv-nano-w / mainline =====
  (kernelTest "mainline")
  # Experimental: same initrd and kernel as kernel-test, with only the
  # full-speed DT cap lifted.  Keep the stable recovery target available.
  (lichee "mainline" [ "kernel-test-hs" ] {
    profile = "usb-kernel-test";
    artifact = "kernel-test";
    tag = "kernel-test-mainline-hs";
    modules = [
      ({ pkgs, ... }: {
        sg2002.fdt = pkgs.sg2002-dtb-mainline-high-speed;
        sg2002.usbGadget.network.transport = "ncm";
      })
    ];
  })
  (debug "mainline")
  (live "mainline" "usb" "live-mainline" { })
  (live "mainline" "usb-rndis" "live-mainline-rndis" (usbTransport "rndis"))
  (live "mainline" "usb-ncm" "live-mainline-ncm" (usbTransport "ncm"))
  (live "mainline" "usb-g-multi" "live-mainline-g-multi" gMulti)
  (live "mainline" "usb-oled" "live-mainline-oled" oled)
  # Historical kink: tag is "live-wifi-<kernel>" not "live-<kernel>-wifi".
  # Kept stable so kexec_target diagnostics don't change.
  (live "mainline" "wifi" "live-wifi-mainline" wifi)

  # ===== licheerv-nano-w / vendor =====
  (kernelTest "vendor")
  (debug "vendor")
  (live "vendor" "usb" "live-vendor" vendorUsb)

  # ===== nanokvm-pcie / vendor =====
  # Production SD image (vendor kernel + vendor-FIT). Network policy
  # belongs in the downstream config that imports the board module.
  (pcie "vendor" [ "sd" ] { profile = "sd-image"; artifact = "sd"; })
  # Initrd-only recovery target using the vendor SDHCI stack. Useful when
  # mainline can reach USB but cannot enumerate the card.
  (pcieKernelTest "vendor")
  # USB-NBD live for hardware bring-up: ethernet (bm-dwmac) + WiFi work
  # natively off the vendor DTS.
  (pcieLive "vendor" "live-pcie-vendor" (pcieLiveExtras // vendorUsb))

  # ===== nanokvm-pcie / mainline =====
  # Initrd-only recovery target for USB/kexec bring-up on the actual PCIe
  # carrier (same DTB as the SD image, but no stage-2 services).
  (pcieKernelTest "mainline")
  (pcie "mainline" [ "kernel-test-hs" ] {
    profile = "usb-kernel-test";
    artifact = "kernel-test";
    tag = "kernel-test-pcie-mainline-hs";
    modules = [
      ({ pkgs, ... }: {
        sg2002.fdt = pkgs.sg2002-dtb-mainline-pcie-high-speed;
        sg2002.usbGadget.network.transport = "ncm";
      })
    ];
  })
  # extlinux SD image (mainline U-Boot). Ethernet via stmmac + the
  # ethernet-enabled DTB; reachable over the USB-ECM gadget too.
  (pcie "mainline" [ "sd" ] { profile = "sd-image-mainline"; artifact = "sd"; })
  # USB-NBD live exercising the full PCIe hardware — eth0 (stmmac) and
  # wlan0 (AIC8800) both come up.
  (pcieLive "mainline" "live-pcie-mainline" pcieLiveExtras)

  # ===== licheerv-nano-w / mainline / NFS over WiFi =====
  # Same WiFi-rooted experiment as the picoclaw wifi entry, on the
  # original dev board (self-cycles its ROM loop on fuckup, so no
  # physical resets while iterating). The AIC8800 is identical.
  (lichee "mainline" [ "live" "wifi-nfs" ] {
    profile = "usb-nfs-live";
    artifact = "nfs-live";
    tag = "live-wifi-nfs-mainline";
    mixins = [
      ../modules/sg2002-initrd-wifi.nix
      ../modules/wifi-aic8800.nix
    ];
    modules = [
      ({ lib, rootWpaConf ? null, ... }: {
        sg2002.wifi.wpaConf = lib.mkDefault rootWpaConf;
        nanokvm.nfsLive.server = "192.168.23.8";
      })
    ];
  })

  # ===== nanokvm-pcie / mainline / NFS over ethernet =====
  # The cleanest data path of all: the PCIe carrier's RJ45. eth0 does
  # DHCP in the initrd (dwmac-sophgo), the root-nfs service mounts
  # trex over the LAN — no dwc2 data, no WiFi. Runs on the router's
  # self-cycling board, so iteration needs no physical resets.
  (pcie "mainline" [ "live" "nfs" ] {
    profile = "usb-nfs-live";
    artifact = "nfs-live";
    tag = "live-pcie-nfs-mainline";
    # Direct USB runners do not inherit boot.kernelParams. Carry the two
    # low-memory stage-2 limits proven on the PicoClaw explicitly.
    artifactArgs.extraBootargs = [
      "systemd.getty_auto=no"
      "udev.children_max=2"
      # The PCIe carrier's 128x64 SSD1306 is usable with the default font,
      # but only as a cramped 16x4 display. Do not rotate this panel.
      "fbcon=font:MINI4x6"
    ];
    mixins = [ ../modules/ethernet.nix ];
    modules = [
      ({ ... }: {
        nanokvm.nfsLive.server = "192.168.23.8";
        sg2002.initrd.availableKernelModules = [
          "stmmac"
          "stmmac_platform"
          "dwmac-sophgo"
        ];
        sg2002.initrd.kernelModules = [ "dwmac-sophgo" ];
        # The store is already mounted over this DHCP lease when initrd
        # networkd hands the interface to stage 2. Preserve it until the new
        # manager has renewed the lease; dropping it deadlocks every uncached
        # executable on the NFS store. Pin the carrier's fleet MAC before the
        # first DHCP request as well, so router assigns its reserved .17.
        boot.initrd.systemd.network.networks."20-eth0" = {
          matchConfig.Name = "eth0";
          networkConfig = {
            DHCP = "yes";
            KeepConfiguration = "dynamic";
          };
          linkConfig = {
            MACAddress = "02:4b:56:4d:00:17";
            RequiredForOnline = "no";
          };
        };
        systemd.network.networks."20-eth0" = {
          networkConfig.KeepConfiguration = "dynamic";
          linkConfig.MACAddress = "02:4b:56:4d:00:17";
        };
      })
    ];
  })

  # ===== licheerv-nano-picoclaw / mainline =====
  # Initrd-only recovery target — the first thing to run on new silicon.
  (picoclawKernelTest "mainline")
  (picoclaw "mainline" [ "kernel-test-hs" ] {
    profile = "usb-kernel-test";
    artifact = "kernel-test";
    tag = "kernel-test-picoclaw-mainline-hs";
    artifactArgs.extraBootargs = [ "cpuidle.off=1" ];
    modules = [
      ({ pkgs, ... }: {
        sg2002.fdt = pkgs.sg2002-dtb-mainline-nowifi-high-speed;
        sg2002.usbGadget.network.transport = "ncm";
      })
    ];
  })
  # USB-booted, NFS-rooted live system (replaces the NBD transport).
  #
  # Bring-up note 2026-07-27: with the WiFi DTB (sdhci1 enabled), the
  # fragile AIC8800 init sequence spins on sdhci1 timeouts and — per
  # the nowifi dtsi's own comment — the SDIO probing contends with the
  # USB gadget for the SoC bus, killing usb0's data path mid-boot.
  # Booting the nowifi DTB avoids that entirely. The wifi-aic8800
  # mixin returns in a follow-up entry once the base boot is solid.
  (picoclawLive "mainline" "live-picoclaw-mainline" {
    # Direct USB runners construct the command line themselves rather than
    # using boot.loader, so carry the getty-generator override explicitly.
    artifactArgs.extraBootargs = [
      "systemd.getty_auto=no"
      "udev.children_max=2"
    ];
    modules = [
      ({ pkgs, ... }: {
        sg2002.fdt = pkgs.sg2002-dtb-mainline-nowifi;
        sg2002.usbGadget.network.transport = "ncm";
      })
    ];
  })

  # Dedicated onboard-LCD sibling of the proven headless USB/NFS boot.
  # It preserves the no-WiFi base and low-memory limits, but swaps in the
  # PicoClaw SPI1/GPIO DTB and runs a persistent ST7789 visible self-test.
  (picoclaw "mainline" [ "live" "usb-lcd" ] {
    profile = "usb-nfs-live";
    artifact = "nfs-live";
    tag = "live-picoclaw-lcd-mainline";
    artifactArgs.extraBootargs = [
      "systemd.getty_auto=no"
      "udev.children_max=2"
    ];
    mixins = [ ../modules/picoclaw-lcd.nix ];
    modules = [
      ({ pkgs, ... }: {
        nanokvm.picoclawLcd.enable = true;
        sg2002.fdt = pkgs.sg2002-dtb-mainline-picoclaw-lcd;
        sg2002.usbGadget.network.transport = "ncm";
      })
    ];
  })

  # High-speed sibling of the LCD/NFS system.  This intentionally keeps the
  # production usb-lcd artifact on its proven full-speed DTB until sustained
  # NFS workloads are verified on the other SG2002 boards too.
  (picoclaw "mainline" [ "live" "usb-lcd-hs" ] {
    profile = "usb-nfs-live";
    artifact = "nfs-live";
    tag = "live-picoclaw-lcd-mainline-hs";
    artifactArgs.extraBootargs = [
      "systemd.getty_auto=no"
      "udev.children_max=2"
    ];
    mixins = [ ../modules/picoclaw-lcd.nix ];
    modules = [
      ({ pkgs, ... }: {
        nanokvm.picoclawLcd.enable = true;
        sg2002.fdt = pkgs.sg2002-dtb-mainline-picoclaw-lcd-high-speed;
        sg2002.usbGadget.network.transport = "ncm";
      })
    ];
  })

  # WiFi-booted variant: the dwc2 gadget net function wedges on this
  # unit (see usb-nfs-live.nix and the bring-up note above), so the
  # store mount rides the AIC8800 over the LAN instead. USB stays on
  # for console + debug shell + kexec control. The NFS export is
  # trex's /export/nix-store, already served to 192.168.23.0/24.
  (picoclaw "mainline" [ "live" "wifi" ] {
    profile = "usb-nfs-live";
    artifact = "nfs-live";
    tag = "live-wifi-picoclaw-mainline";
    mixins = [
      ../modules/sg2002-initrd-wifi.nix
      ../modules/wifi-aic8800.nix
    ];
    modules = [
      ({ lib, rootWpaConf ? null, ... }: {
        sg2002.wifi.wpaConf = lib.mkDefault rootWpaConf;
        # NFS root over the LAN, served by trex. Runtime override:
        # NANOKVM_NFS_SERVER env → nanokvm.nfs_server= cmdline arg.
        nanokvm.nfsLive.server = "192.168.23.8";
      })
    ];
  })
]
