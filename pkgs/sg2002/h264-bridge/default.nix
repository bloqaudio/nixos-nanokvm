{ stdenv }:

stdenv.mkDerivation {
  pname = "sg2002-h264-bridge";
  version = "0.1";

  src = ./sg2002-h264-bridge.c;
  dontUnpack = true;
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -O2 -Wall -Wextra -Wconversion -Wshadow -Wformat=2 \
      -Werror -o sg2002-h264-bridge $src
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 sg2002-h264-bridge "$out/bin/sg2002-h264-bridge"
    runHook postInstall
  '';
}
