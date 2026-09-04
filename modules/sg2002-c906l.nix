{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.sg2002.auxCore;
  controlModule = pkgs.sg2002-c906l-control-for config.boot.kernelPackages.kernel;
  remoteprocModule = pkgs.sg2002-c906l-remoteproc-for config.boot.kernelPackages.kernel;
  memoryMap = import ../pkgs/sg2002/c906l-memory-map.nix;
  inherit (memoryMap) firmwareAddress sharedMemoryAddress;
  carveoutSize = memoryMap.firmwareSize;
  firmwareContractFields = [
    "firmwareAddress"
    "firmwareFile"
    "firmwareSize"
    "enabledPeripherals"
    "protocolVersion"
    "requiredCapabilities"
    "sharedMemoryAddress"
    "sharedMemorySize"
  ];
  firmwareHasContract = lib.all (name: builtins.hasAttr name cfg.firmware) firmwareContractFields;
  expectedCapabilities = 11
    + (if builtins.elem "timer4" cfg.peripherals then 4 else 0);
  firmwareContractMatches = firmwareHasContract
    && cfg.firmware.firmwareAddress == firmwareAddress
    && cfg.firmware.firmwareSize == carveoutSize
    && cfg.firmware.enabledPeripherals == lib.sort builtins.lessThan (lib.unique cfg.peripherals)
    && cfg.firmware.protocolVersion.major == 1
    && cfg.firmware.requiredCapabilities == expectedCapabilities
    && cfg.firmware.sharedMemoryAddress == sharedMemoryAddress
    && cfg.firmware.sharedMemorySize == memoryMap.sharedMemorySize;
  fdtContractFields = [
    "firmwareAddress"
    "firmwareSize"
    "sharedMemoryAddress"
    "sharedMemorySize"
  ];
  fdtHasContract = lib.all (name: builtins.hasAttr name cfg.fdt) fdtContractFields;
  fdtContractMatches = fdtHasContract
    && cfg.fdt.firmwareAddress == firmwareAddress
    && cfg.fdt.firmwareSize == carveoutSize
    && cfg.fdt.sharedMemoryAddress == sharedMemoryAddress
    && cfg.fdt.sharedMemorySize == memoryMap.sharedMemorySize;
in
{
  options.sg2002.auxCore = {
    enable = lib.mkEnableOption "the SG2002 C906L real-time auxiliary core";

    peripherals = lib.mkOption {
      type = lib.types.listOf (lib.types.enum [ "timer4" ]);
      default = [ ];
      example = [ "timer4" ];
      description = ''
        Statically leased C906L peripherals.  Each selection enables its Rust
        driver and C interrupt glue in the same firmware derivation.  There is
        intentionally no wildcard: every new lease needs an ownership audit,
        DT review, bounded self-test, and explicit capability bit.
      '';
    };

    firmware = lib.mkOption {
      type = lib.types.package;
      default = pkgs.sg2002-c906l-firmware-for cfg.peripherals;
      defaultText = lib.literalExpression
        "pkgs.sg2002-c906l-firmware-for config.sg2002.auxCore.peripherals";
      description = ''
        C906L firmware package.  It must publish the complete memory layout,
        enabled peripheral set, required capabilities, and protocol version
        passthru contract understood by the FIP, runner, DT, and Linux side.
      '';
    };

    fdt = lib.mkOption {
      type = lib.types.package;
      default = pkgs.sg2002-dtb-mainline-nowifi-c906l;
      defaultText = lib.literalExpression "pkgs.sg2002-dtb-mainline-nowifi-c906l";
      description = ''
        Board-composed device tree containing the C906L reservations and
        transport nodes.  Board modules with additional carrier hardware must
        override this default with their matching composed DT; substituting a
        generic development-board DT can silently remove unrelated devices.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = config.sg2002.kernel == "mainline";
        message = "sg2002.auxCore currently supports only the mainline kernel";
      }
      {
        assertion = config.sg2002.uboot == "mainline";
        message = "sg2002.auxCore requires the verified mainline FIP composition path";
      }
      {
        assertion = !config.sg2002.wifi.enable;
        message = ''
          sg2002.auxCore currently selects the validated no-WiFi DT variant;
          disable sg2002.wifi until composable C906L DT overlays are packaged
        '';
      }
      {
        assertion = cfg.peripherals == lib.unique cfg.peripherals;
        message = "sg2002.auxCore.peripherals must not contain duplicates";
      }
      {
        assertion = firmwareHasContract;
        message = ''
          sg2002.auxCore.firmware must expose firmwareAddress, firmwareFile,
          firmwareSize, enabledPeripherals, protocolVersion,
          requiredCapabilities,
          sharedMemoryAddress, and sharedMemorySize passthru attributes
        '';
      }
      {
        assertion = firmwareContractMatches;
        message = ''
          sg2002.auxCore.firmware must match the DT contract: firmware at
          0x8fe00000/1MiB, shared memory at 0x8ff00000/1MiB, ABI major 1,
          and exactly the configured peripheral lease set
        '';
      }
      {
        assertion = fdtHasContract;
        message = ''
          sg2002.auxCore.fdt must expose firmwareAddress, firmwareSize,
          sharedMemoryAddress, and sharedMemorySize passthru attributes
        '';
      }
      {
        assertion = fdtContractMatches;
        message = ''
          sg2002.auxCore.fdt must reserve the exact C906L firmware and shared
          memory ranges selected by the firmware contract
        '';
      }
    ];

    # Firmware selection, FIP packing, and the Linux memory reservation move
    # together.  A partially enabled auxiliary core could let Linux's CMA
    # allocator overwrite executing firmware, so these are deliberately
    # non-overridable while the option is enabled.
    system.build.c906lFirmware = cfg.firmware;
    system.build.fip = lib.mkForce (pkgs.sg2002-fip-mainline-uboot-for cfg.firmware);
    system.build.fipFastboot = lib.mkForce (pkgs.sg2002-fip-mainline-fastboot-for cfg.firmware);
    sg2002.fdt = lib.mkForce cfg.fdt;
    boot.extraModulePackages = [
      controlModule
      remoteprocModule
    ];
    boot.kernelModules = [
      "sg2002-c906l-control"
      "sg2002-c906l-remoteproc"
    ];
    environment.systemPackages = [ pkgs.sg2002-c906l-ctl ];
  };
}
