{ lib, ... }:

{
  nixpkgs = {
    buildPlatform = lib.mkDefault "x86_64-linux";
    hostPlatform = lib.mkDefault "riscv64-linux";
    config = {
      allowBroken = lib.mkDefault true;
      allowUnfree = lib.mkDefault true;
      allowUnsupportedSystem = lib.mkDefault true;
    };
  };
}
