{ pkgs, config, artifactArgs, picoclawFdtMismatch }:

let
  packageNames = map pkgs.lib.getName config.environment.systemPackages;
  extraModuleNames = map pkgs.lib.getName config.boot.extraModulePackages;
in
assert config.sg2002.auxCore.enable;
assert config.sg2002.auxCore.peripherals == [ "picoclawLcd" ];
assert config.sg2002.auxCore.firmware.profileName == "picoclaw-lcd";
assert config.sg2002.auxCore.fdt.profileName == "picoclaw-lcd";
assert config.sg2002.auxCore.fdt.boardProfile == "picoclaw-c906l-lcd";
assert config.system.build.fip.c906lContract.profileName == "picoclaw-lcd";
assert config.system.build.fipFastboot.c906lContract.profileName == "picoclaw-lcd";
assert config.sg2002.watchdogKeeper.initrd.enable;
assert config.sg2002.watchdogKeeper.stage2.enable;
assert config.sg2002.watchdogKeeper.healthHost == "10.55.0.2";
assert !config.sg2002.usbGadget.console.enable;
assert config.sg2002.usbGadget.network.enable;
assert config.nanokvm.usbControl.initrd.enable;
assert config.nanokvm.usbControl.stage2.enable;
assert artifactArgs.usbConsole == false;
assert !config.sg2002.wifi.enable;
assert !config.services.nanokvm.enable;
assert !config.services.openssh.enable;
assert builtins.length packageNames == 5;
assert builtins.elem "sg2002-c906l-ctl-picoclaw-lcd" packageNames;
assert builtins.elem "sg2002-c906l-drm-test" packageNames;
assert builtins.elem "sg2002-c906l-control" extraModuleNames;
assert builtins.elem "sg2002-c906l-remoteproc" extraModuleNames;
assert builtins.elem "sg2002-c906l-framebuffer" extraModuleNames;
assert builtins.elem "sg2002-c906l-framebuffer" config.boot.kernelModules;
assert !picoclawFdtMismatch.success;
pkgs.runCommand "sg2002-c906l-picoclaw-module-eval" { } ''
  touch "$out"
''
