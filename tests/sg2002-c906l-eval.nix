{ pkgs
, lib
, config
, firmwareMismatch
, fdtMismatch
, duplicatePeripherals
}:

let
  expectedPeripherals = [ "timer4" "timer5" "timer6" "timer7" ];
  expectedProfile = "all-timers";
  packageNames = map lib.getName config.environment.systemPackages;
in
assert config.sg2002.auxCore.enable;
assert config.sg2002.auxCore.peripherals == expectedPeripherals;
assert config.sg2002.auxCore.firmware.enabledPeripherals == expectedPeripherals;
assert config.sg2002.auxCore.firmware.profileName == expectedProfile;
assert config.sg2002.auxCore.fdt.profileName == expectedProfile;
assert config.system.build.fip.c906lContract.profileName == expectedProfile;
assert config.system.build.fipFastboot.c906lContract.profileName == expectedProfile;
assert config.sg2002.auxCore.firmware.contractSha256
  == config.sg2002.auxCore.fdt.contractSha256;
assert config.sg2002.auxCore.firmware.contractSha256
  == config.system.build.fip.c906lContract.contractSha256;
assert config.sg2002.watchdogKeeper.initrd.enable;
assert config.sg2002.watchdogKeeper.stage2.enable;
assert config.sg2002.watchdogKeeper.healthHost == "10.55.0.2";
assert !config.services.nanokvm.enable;
assert !config.services.openssh.enable;
assert builtins.length packageNames == 4;
assert builtins.elem "sg2002-c906l-ctl-all-timers" packageNames;
assert !firmwareMismatch.success;
assert !fdtMismatch.success;
assert !duplicatePeripherals.success;
pkgs.runCommand "sg2002-c906l-module-eval" { } ''
  touch "$out"
''
