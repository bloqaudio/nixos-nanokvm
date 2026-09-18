{ pkgs, config }:

let
  requiredInitrdModules = [
    "mmc_block"
    "sg2002-c906l-control"
    "sg2002-c906l-remoteproc"
    "sg2002-c906l-wifi-power"
  ];
in
assert config.sg2002.auxCore.enable;
assert config.sg2002.auxCore.peripherals == [ "picoclawLcd" ];
assert config.sg2002.auxCore.fdt.boardProfile == "picoclaw-c906l-lcd";
assert config.system.build.fip.c906lContract.profileName == "picoclaw-lcd";
assert config.system.build.sdImage != null;
assert config.sg2002.watchdogKeeper.initrd.enable;
assert config.sg2002.watchdogKeeper.stage2.enable;
assert config.sg2002.watchdogKeeper.healthHost == null;
assert config.sg2002.wifi.enable;
assert config.sg2002.wifi.wpaConfRuntimePath
  == "/etc/wpa_supplicant/wpa_supplicant-wlan0.conf";
assert !config.sg2002.usbGadget.console.enable;
assert !builtins.elem "console=ttyGS0,115200" config.boot.kernelParams;
assert builtins.elem "console=tty0" config.boot.kernelParams;
assert builtins.elem "fbcon=nodefer" config.boot.kernelParams;
assert builtins.elem "sg2002-c906l-framebuffer" config.sg2002.initrd.availableKernelModules;
assert !builtins.elem "sg2002-c906l-framebuffer" config.sg2002.initrd.kernelModules;
assert config.fileSystems."/".fsType == "btrfs";
assert config.boot.loader.generic-extlinux-compatible.enable;
assert config.boot.initrd.systemd.services.initrd-switch-root.enable;
assert config.systemd.services."getty@".enable;
assert builtins.elem "getty@tty1.service" config.systemd.targets.getty.wants;
assert config.services.openssh.enable;
assert config.services.openssh.settings.PermitRootLogin == "yes";
assert config.services.openssh.settings.PasswordAuthentication;
assert !config.services.openssh.settings.KbdInteractiveAuthentication;
assert config.users.mutableUsers;
assert config.users.users.root.initialPassword == null;
assert pkgs.lib.hasPrefix "$6$" config.users.users.root.hashedPassword;
assert pkgs.lib.all
  (module: builtins.elem module config.sg2002.initrd.availableKernelModules)
  requiredInitrdModules;
assert pkgs.lib.all
  (module: builtins.elem module config.sg2002.initrd.kernelModules)
  requiredInitrdModules;
pkgs.runCommand "sg2002-c906l-picoclaw-sd-module-eval" { } ''
  # Exercise the generated installer, not a second copy of its policy.
  ${pkgs.gnugrep}/bin/grep -F 'btrfs property set -t inode /boot/extlinux compression none' \
    ${config.system.build.installBootLoader}
  ${pkgs.gnugrep}/bin/grep -F 'btrfs property set -t inode /boot/nixos compression none' \
    ${config.system.build.installBootLoader}
  touch "$out"
''
