{
  imports = [
    ../platform/riscv64-cross.nix
    ../platform/spacemit-k3.nix
  ];

  spacemit.k3.enable = true;
}
