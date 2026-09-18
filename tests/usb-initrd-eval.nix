{ pkgs, lib, configs }:
let
  check = config:
    let
      stage1 = config.boot.initrd;
      net = stage1.systemd.network.networks."10-all-links";
      services = stage1.systemd.services;
      lcd = builtins.elem "picoclawLcd" config.sg2002.auxCore.peripherals;
      consoles = builtins.filter (lib.hasPrefix "console=") config.boot.kernelParams;
    in
    assert lib.all (a: a.assertion) config.assertions;
    assert stage1.systemd.enable;
    assert stage1.systemd.root == null;
    assert stage1.network.ssh.enable;
    assert !stage1.systemd.emergencyAccess;
    assert stage1.network.ssh.authorizedKeys != [ ];
    assert stage1.network.ssh.hostKeys == [ ];
    assert !services.initrd-switch-root.enable;
    assert !services.initrd-cleanup.enable;
    assert !stage1.systemd.targets.initrd-switch-root.enable;
    assert !builtins.hasAttr "usb-debug-shell" services;
    assert !builtins.hasAttr "nanokvm-root-nbd" services;
    assert !builtins.hasAttr "nanokvm-nfs-store" services;
    assert !builtins.hasAttr "kexec" stage1.systemd.sockets;
    assert !stage1.systemd.network.wait-online.enable;
    assert net.networkConfig.DHCP == "yes";
    assert net.networkConfig.LinkLocalAddressing == "yes";
    assert net.matchConfig.Name == [ "eth*" "en*" "wl*" "usb*" ];
    assert stage1.services.resolved.enable;
    assert config.sg2002.watchdogKeeper.initrd.enable;
    assert config.sg2002.watchdogKeeper.healthHost == null;
    assert (config.sg2002.wifi.enable && config.sg2002.wifi.wpaConf == null
      && config.sg2002.wifi.wpaConfRuntimePath != null) ->
      services.wpa_supplicant-wlan0.unitConfig.ConditionPathExists
        == config.sg2002.wifi.wpaConfRuntimePath;
    assert !config.sg2002.watchdogKeeper.stage2.enable;
    assert !config.sg2002.usbGadget.stage2.enable;
    assert builtins.elem "console=tty0" consoles == lcd;
    assert lib.last consoles == "console=${if config.sg2002.uart1Rescue.enable then "ttyS1" else "ttyS0"},115200";
    assert !lib.any (p: lib.hasPrefix "init=" p || lib.hasPrefix "root=" p) config.boot.kernelParams;
    assert stage1.systemd.extraBin ? aplay;
    assert stage1.systemd.extraBin ? arecord;
    assert stage1.systemd.extraBin.systemd-analyze == "${stage1.systemd.package}/bin/systemd-analyze";
    true;
in
assert lib.all check configs;
pkgs.runCommand "sg2002-initrd-eval" { } ''
  touch "$out"
''
