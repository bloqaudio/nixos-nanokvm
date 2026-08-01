# PicoClaw onboard 240x240 ST7789 LCD.  Hardware pinmux and the SPI child
# live in the dedicated picoclaw-lcd DTB; this module loads the mainline
# DesignWare/spidev stack and paints an unmistakable RGB/text test pattern.
{ config, lib, pkgs, ... }:
let
  cfg = config.nanokvm.picoclawLcd;
  watchdogKeeper = pkgs.pkgsStatic.busybox;
  watchdogKeeperService = {
    description = "Keep the PicoClaw hardware watchdog alive across switch-root";
    after = [ "systemd-udevd.service" ];
    unitConfig = {
      DefaultDependencies = false;
      IgnoreOnIsolate = true;
      RefuseManualStop = true;
      SurviveFinalKillSignal = true;
    };
    serviceConfig = {
      Type = "simple";
      ExecStart = "${watchdogKeeper}/bin/busybox watchdog -F -t 5 -T 85 /dev/watchdog0";
      Restart = "always";
      RestartSec = "250ms";
    };
  };
  waitForSpi = pkgs.writeShellScript "picoclaw-lcd-wait-for-spi" ''
    for _ in $(${pkgs.coreutils}/bin/seq 1 100); do
      if [ -c ${lib.escapeShellArg cfg.spiDevice} ]; then
        exit 0
      fi
      ${pkgs.coreutils}/bin/sleep 0.1
    done

    echo "PicoClaw LCD SPI device did not appear: ${cfg.spiDevice}" >&2
    exit 1
  '';
in
{
  options.nanokvm.picoclawLcd = {
    enable = lib.mkEnableOption "PicoClaw onboard ST7789 LCD test service";

    spiDevice = lib.mkOption {
      type = lib.types.str;
      default = "/dev/spidev1.0";
      description = "spidev node for the PicoClaw ST7789 panel.";
    };

    gpioChip = lib.mkOption {
      type = lib.types.str;
      default = "/dev/gpiochip0";
      description = "GPIO character device containing GPIOA19/A27/A28.";
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = config.sg2002.kernel == "mainline";
        message = "nanokvm.picoclawLcd currently supports only the mainline SG2002 kernel";
      }
    ];

    sg2002.fdt = lib.mkDefault pkgs.sg2002-dtb-mainline-picoclaw-lcd;

    # SPI_DESIGNWARE, SPI_DW_MMIO and SPI_SPIDEV are explicit kernel-config
    # contracts in linux-mainline/config.nix.  The systemd initrd keeps
    # systemd-modules-load.service active across switch-root, so stage 2 does
    # not replay boot.kernelModules; include and load the stack in stage 1.
    # The three compressed modules add only about 42 KiB to the initrd and
    # make /dev/spidev1.0 ready before the NFS-rooted service starts.
    sg2002.initrd.availableKernelModules = [
      "spi-dw-mmio"
      "spidev"
    ];
    sg2002.initrd.kernelModules = [
      "spi-dw-mmio"
      "spidev"
    ];
    boot.kernelModules = [ "spi-dw-mmio" "spidev" ];

    # PID 1 can spend longer than the DesignWare watchdog's 85.9-second
    # hardware maximum blocked on USB/NFS page faults during switch-root.
    # Do not make the blocked process its own watchdog client.  A static
    # BusyBox process started in the initrd owns and pets the device instead;
    # it stays runnable without the remote store and survives switch-root.
    boot.initrd.systemd.settings.Manager.RuntimeWatchdogSec = lib.mkForce "off";
    systemd.settings.Manager.RuntimeWatchdogSec = lib.mkForce "off";

    boot.initrd.systemd.storePaths = [ watchdogKeeper ];
    boot.initrd.systemd.services.picoclaw-watchdog-keeper = watchdogKeeperService // {
      wantedBy = [ "initrd.target" ];
    };
    # Keep the same unit definition available after the manager switches
    # root, so systemd can carry its cgroup and main process forward.
    systemd.services.picoclaw-watchdog-keeper = watchdogKeeperService // {
      wantedBy = [ "multi-user.target" ];
    };

    systemd.services.picoclaw-lcd-test = {
      description = "PicoClaw ST7789 visible LCD self-test";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" ];
      serviceConfig = {
        Type = "simple";
        ExecStartPre = [ waitForSpi ];
        ExecStart = "${pkgs.picoclaw-lcd-test}/bin/picoclaw-lcd-test ${cfg.spiDevice} ${cfg.gpioChip}";
        Restart = "on-failure";
        RestartSec = "5s";
      };
    };
  };
}
