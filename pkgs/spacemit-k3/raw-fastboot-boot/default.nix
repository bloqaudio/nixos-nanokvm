{
  lib,
  makeWrapper,
  python3,
  python3Packages,
  stdenvNoCC,
}: let
  pythonEnv = python3.withPackages (_: [
    python3Packages.pyusb
  ]);
in
  stdenvNoCC.mkDerivation {
    pname = "spacemit-k3-raw-fastboot-boot";
    version = "0.1.0";

    src = ./raw-fastboot-boot.py;
    dontUnpack = true;

    nativeBuildInputs = [makeWrapper];

    installPhase = ''
      runHook preInstall

      install -Dm755 "$src" "$out/libexec/spacemit-k3-raw-fastboot-boot.py"
      makeWrapper ${pythonEnv}/bin/python3 "$out/bin/spacemit-k3-raw-fastboot-boot" \
        --add-flags "$out/libexec/spacemit-k3-raw-fastboot-boot.py"

      runHook postInstall
    '';

    meta = {
      description = "Send a raw FIT image to SpacemiT K3 U-Boot fastboot and boot it";
      license = lib.licenses.mit;
      maintainers = [lib.maintainers.georgewhewell];
      platforms = lib.platforms.linux;
    };
  }
