{ stdenv, alsa-lib }:

stdenv.mkDerivation {
  pname = "sg2002-alsa-kernel-test";
  version = "1";
  src = ../../../artifacts/camera-lab/alsa-kernel-test.c;
  dontUnpack = true;
  dontConfigure = true;

  buildInputs = [ alsa-lib ];

  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -O2 -Wall -Wextra -Wconversion -Wshadow -Wformat=2 \
      -Werror -o sg2002-alsa-kernel-test $src -lasound -lm
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 sg2002-alsa-kernel-test "$out/bin/sg2002-alsa-kernel-test"
    runHook postInstall
  '';
}
