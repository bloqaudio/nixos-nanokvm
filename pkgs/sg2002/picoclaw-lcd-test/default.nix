{ stdenv }:

stdenv.mkDerivation {
  pname = "picoclaw-lcd-test";
  version = "1";

  src = ./.;
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -O2 -Wall -Wextra -Werror picoclaw-lcd-test.c -o picoclaw-lcd-test
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 picoclaw-lcd-test "$out/bin/picoclaw-lcd-test"
    runHook postInstall
  '';
}
