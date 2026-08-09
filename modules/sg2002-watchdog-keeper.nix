# Keep the SG2002 DesignWare watchdog alive independently of systemd PID 1.
# Slow remote-root boots can block the manager on page faults for longer than
# the watchdog's roughly 86-second hardware maximum.
{ config, lib, pkgs, ... }:
let
  cfg = config.sg2002.watchdogKeeper;
  keeper = pkgs.pkgsStatic.busybox;
  keeperService = {
    description = "Keep the SG2002 hardware watchdog alive across switch-root";
    after = [ "systemd-udevd.service" ];
    unitConfig = {
      DefaultDependencies = false;
      IgnoreOnIsolate = true;
      RefuseManualStop = true;
      SurviveFinalKillSignal = true;
    };
    serviceConfig = {
      Type = "simple";
      ExecStart = "${keeper}/bin/busybox watchdog -F -t 5 -T 85 /dev/watchdog0";
      Restart = "always";
      RestartSec = "250ms";
    };
  };
in
{
  options.sg2002.watchdogKeeper = {
    initrd.enable = lib.mkEnableOption "the independent SG2002 initrd watchdog keeper";
    stage2.enable = lib.mkEnableOption "the independent SG2002 stage-2 watchdog keeper";
  };

  config = lib.mkMerge [
    {
      assertions = [
        {
          assertion = !cfg.stage2.enable || cfg.initrd.enable;
          message = "sg2002.watchdogKeeper.stage2.enable requires initrd.enable";
        }
        {
          assertion = !cfg.initrd.enable || config.boot.initrd.systemd.enable;
          message = "sg2002.watchdogKeeper.initrd.enable requires a systemd initrd";
        }
      ];
    }

    (lib.mkIf cfg.initrd.enable {
      # The keeper, rather than a possibly blocked PID 1, owns watchdog0.
      boot.initrd.systemd.settings.Manager.RuntimeWatchdogSec = lib.mkForce "off";
      boot.initrd.systemd.storePaths = [ keeper ];
      boot.initrd.systemd.services.sg2002-watchdog-keeper = keeperService // {
        wantedBy = [ "initrd.target" ];
      };
    })

    (lib.mkIf cfg.stage2.enable {
      # Keep the same unit definition after switch-root so systemd carries
      # the initrd process and its watchdog file descriptor forward.
      systemd.settings.Manager.RuntimeWatchdogSec = lib.mkForce "off";
      systemd.services.sg2002-watchdog-keeper = keeperService // {
        wantedBy = [ "multi-user.target" ];
      };
    })
  ];
}
