{
  buildUBoot,
  fetchFromGitHub,
  lib,
}:

buildUBoot {
  version = "2022.10-k3-br-v1.0.y";

  src = fetchFromGitHub {
    owner = "liberodark";
    repo = "spacemit-uboot-2022.10";
    rev = "368953aa0b648528ddafa5b6baa73507448ab94d";
    hash = "sha256-LTtxHzL5aoMLlv8DsgEoyIM0JrwYYzNlHCAmXzixScI=";
  };

  defconfig = "k3_defconfig";

  filesToInstall = [
    "FSBL.bin"
    "bootinfo_block.bin"
    "bootinfo_spinor.bin"
    "bootinfo_spinand.bin"
  ];

  extraMeta = {
    description = "First-stage bootloader artifacts for SpacemiT K3";
    homepage = "https://github.com/liberodark/spacemit-uboot-2022.10";
    license = lib.licenses.gpl2Only;
    maintainers = [ lib.maintainers.georgewhewell ];
    platforms = [ "riscv64-linux" ];
  };
}
