{
  fetchFromGitHub,
  lib,
  stdenvNoCC,
}:

stdenvNoCC.mkDerivation {
  pname = "spacemit-k3-uefi-flash";
  version = "2026-06-30-9f613a6";

  src = fetchFromGitHub {
    owner = "liberodark";
    repo = "k3-uefi-flash";
    rev = "9f613a666d904d351ea98db65e94cb13dd54ca0c";
    hash = "sha256-Fu07q/PaPeTdIF3jjB7vMb9j8MwJpqzao8eNVN8JNn0=";
  };

  dontConfigure = true;
  dontBuild = true;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/share/spacemit-k3-uefi-flash/factory
    cp SHA256SUMS edk2.itb env.bin esos.itb flash.sh fw_dynamic.itb partition_4M.json u-boot.itb \
      $out/share/spacemit-k3-uefi-flash/
    cp factory/FSBL.bin factory/bootinfo_spinor.bin \
      $out/share/spacemit-k3-uefi-flash/factory/

    runHook postInstall
  '';

  meta = {
    description = "EDK2/OpenSBI/U-Boot flash bundle for SpacemiT K3 Pico-ITX";
    homepage = "https://github.com/liberodark/k3-uefi-flash";
    license = lib.licenses.unfreeRedistributableFirmware;
    maintainers = [ lib.maintainers.georgewhewell ];
    platforms = lib.platforms.all;
  };
}
