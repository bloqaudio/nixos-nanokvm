{ pkgs, config, artifactArgs, picoclawFdtMismatch }:

let
  packageNames = map pkgs.lib.getName config.environment.systemPackages;
  extraModuleNames = map pkgs.lib.getName config.boot.extraModulePackages;
  consoles = builtins.filter (pkgs.lib.hasPrefix "console=") config.boot.kernelParams;
in
assert config.sg2002.auxCore.enable;
assert config.sg2002.auxCore.peripherals == [ "picoclawLcd" ];
assert config.sg2002.auxCore.firmware.profileName == "picoclaw-lcd";
assert config.sg2002.auxCore.fdt.profileName == "picoclaw-lcd";
assert config.sg2002.auxCore.fdt.boardProfile == "picoclaw-c906l-lcd";
assert config.system.build.fip.c906lContract.profileName == "picoclaw-lcd";
assert config.system.build.fipFastboot.c906lContract.profileName == "picoclaw-lcd";
assert config.sg2002.watchdogKeeper.initrd.enable;
assert !config.sg2002.watchdogKeeper.stage2.enable;
assert config.sg2002.watchdogKeeper.healthHost == null;
assert !config.sg2002.usbGadget.console.enable;
assert config.sg2002.usbGadget.network.enable;
assert config.sg2002.wifi.enable;
assert config.sg2002.auxCore.fdt.wifiPowerProvider == "c906l-regulator";
assert !config.services.nanokvm.enable;
assert !config.services.openssh.enable;
assert !config.zramSwap.enable;
assert builtins.elem "systemd.getty_auto=no" config.boot.kernelParams;
assert builtins.elem "udev.children_max=2" config.boot.kernelParams;
assert consoles == [ "console=tty0" "console=ttyS0,115200" ];
assert builtins.elem "fbcon=nodefer" config.boot.kernelParams;
assert builtins.elem "fbcon=font:MINI4x6" config.boot.kernelParams;
assert builtins.elem "consoleblank=0" config.boot.kernelParams;
assert builtins.elem "loglevel=7" config.boot.kernelParams;
assert builtins.elem "sg2002-c906l-framebuffer" config.boot.initrd.availableKernelModules;
assert !(builtins.elem "sg2002-c906l-framebuffer" config.boot.initrd.kernelModules);
assert !config.boot.initrd.systemd.emergencyAccess;
assert !(builtins.hasAttr "getty@tty1" config.boot.initrd.systemd.services);
assert builtins.elem "sg2002-c906l-ctl-picoclaw-lcd" packageNames;
assert builtins.elem "sg2002-c906l-drm-test" packageNames;
assert builtins.elem "sg2002-c906l-control" extraModuleNames;
assert builtins.elem "sg2002-c906l-remoteproc" extraModuleNames;
assert builtins.elem "sg2002-c906l-framebuffer" extraModuleNames;
assert builtins.elem "sg2002-c906l-wifi-power" extraModuleNames;
assert builtins.elem "sg2002-c906l-wifi-power" config.boot.kernelModules;
assert builtins.elem "sg2002-c906l-framebuffer" config.boot.kernelModules;
assert !picoclawFdtMismatch.success;
pkgs.runCommand "sg2002-c906l-picoclaw-module-eval" { } ''
  touch "$out"
''
