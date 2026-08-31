# AIC8800 WiFi in the systemd initrd. When this module is imported,
# the aic8800 modules + wpa_supplicant bring up wlan0 before stage-2.
# Requires `sg2002.wifi.enable = true` and, for association,
# `sg2002.wifi.wpaConf`.
#
# Pair this with an initrd boot profile such as profiles/usb-nfs-live.nix.
# modules/wifi-aic8800.nix owns the corresponding stage-2 service.
{
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.sg2002;

  kernelPkg =
    if cfg.kernel == "mainline" && cfg.audio.enable && cfg.bluetooth.enable
    then pkgs.sg2002-kernel-mainline-audio-bluetooth
    else if cfg.kernel == "mainline" && cfg.audio.enable
    then pkgs.sg2002-kernel-mainline-audio
    else if cfg.kernel == "mainline" && cfg.bluetooth.enable
    then pkgs.sg2002-kernel-mainline-bluetooth
    else pkgs."sg2002-kernel-${cfg.kernel}";
  aic8800Pkg =
    if cfg.kernel == "vendor"
    then pkgs.sg2002-aic8800-vendor-for kernelPkg
    else if cfg.bluetooth.enable
    then pkgs.sg2002-aic8800-mainline-bluetooth-for kernelPkg
    else pkgs.sg2002-aic8800-mainline-for kernelPkg;
  runtimeWpaConf = cfg.wifi.wpaConfRuntimePath;
  manageInitrd = cfg.wifi.wpaConf != null || runtimeWpaConf != null;
  wpaConfPath =
    if runtimeWpaConf == null
    then "/etc/wpa_supplicant.conf"
    else runtimeWpaConf;
in {
  config = lib.mkMerge [
    {
      assertions = [
        {
          assertion = cfg.wifi.enable;
          message = "initrd-wifi.nix requires sg2002.wifi.enable = true.";
        }
        {
          assertion =
            runtimeWpaConf == null
            || (lib.hasPrefix "/run/" runtimeWpaConf
              && builtins.match "/run/[^/]+" runtimeWpaConf != null);
          message = "sg2002.wifi.wpaConfRuntimePath must be a direct child of /run.";
        }
      ];

      boot.extraModulePackages = [aic8800Pkg];
      sg2002.initrd.pruneKernelModules = true;
      sg2002.initrd.availableKernelModules = lib.optionals cfg.bluetooth.enable [ "bluetooth" "bnep" "rfcomm" ] ++ [
        "aic8800_bsp"
        "aic8800_fdrv"
        "aic8800_btlpm"
      ];
      sg2002.initrd.kernelModules = lib.optionals cfg.bluetooth.enable [ "bluetooth" "bnep" "rfcomm" ] ++ [
        "aic8800_bsp"
        "aic8800_fdrv"
        "aic8800_btlpm"
      ];
      hardware.firmware = [pkgs.sg2002-aic8800-firmware];

      # NixOS's systemd initrd defaults /lib -> modulesClosure/lib
      # (modules only, empty firmware dir). aicbsp opens
      # /lib/firmware/... directly via filp_open — not
      # request_firmware — so the initrd must have those files at
      # the exact /lib/firmware path. Replace /lib with a merged
      # tree that carries both the module tree and our firmware
      # blobs at build time: systemd-modules-load runs before
      # systemd-tmpfiles-setup, so a runtime symlink would be too
      # late.
      boot.initrd.systemd.contents."/lib".source = lib.mkForce (
        pkgs.runCommand "sg2002-initrd-lib" {} ''
          mkdir -p $out
          ln -s ${config.system.build.modulesClosure}/lib/modules $out/modules
          ln -s ${config.hardware.firmware}/lib/firmware $out/firmware
        ''
      );
    }

    (lib.mkIf manageInitrd {
      boot.initrd.systemd = {
        contents = lib.mkIf (cfg.wifi.wpaConf != null) {
          "/etc/wpa_supplicant.conf".text = cfg.wifi.wpaConf;
        };

        services."wpa_supplicant-wlan0" = {
          description = "wpa_supplicant on wlan0";
          wantedBy = ["initrd.target"];
          after = [
            "systemd-modules-load.service"
            "sys-subsystem-net-devices-wlan0.device"
          ];
          wants = ["sys-subsystem-net-devices-wlan0.device"];
          # A remote store carried by wlan0 cannot tolerate the normal
          # switch-root final kill: stage 2 would have to page in a fresh
          # wpa_supplicant over the association that was just torn down.
          # Keep this instance alive so the identically named stage-2 unit
          # can adopt it without an NFS connectivity gap.
          unitConfig = {
            IgnoreOnIsolate = true;
            SurviveFinalKillSignal = true;
          };
          serviceConfig = {
            ExecStartPre = lib.optionals (runtimeWpaConf != null && cfg.wifi.wpaConf != null) [
              "${pkgs.busybox}/bin/busybox cp -f /etc/wpa_supplicant.conf ${wpaConfPath}"
              "${pkgs.busybox}/bin/busybox chmod 0600 ${wpaConfPath}"
            ];
            ExecStart = "${pkgs.wpa_supplicant}/bin/wpa_supplicant -i wlan0 -c ${wpaConfPath} -D nl80211";
            Restart = "on-failure";
            RestartSec = 5;
          };
        };

        storePaths = [
          "${pkgs.wpa_supplicant}/bin/wpa_supplicant"
        ] ++ lib.optional (runtimeWpaConf != null) pkgs.busybox;

        network.networks."40-wlan0" = {
          matchConfig.Name = "wlan0";
          networkConfig.DHCP = "yes";
        };
      };
    })
  ];
}
