{ lib, stdenv, pkg-config, libdrm }:
stdenv.mkDerivation {
  pname = "sg2002-c906l-drm-test";
  version = "0.1.0";
  src = lib.fileset.toSource { root = ./.; fileset = ./drm-test.c; };
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libdrm ];
  strictDeps = true;
  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -Wall -Wextra -Werror -O2 $($PKG_CONFIG --cflags libdrm) \
      drm-test.c -o sg2002-c906l-drm-test $($PKG_CONFIG --libs libdrm)
    runHook postBuild
  '';
  installPhase = ''
    install -Dm0755 sg2002-c906l-drm-test "$out/bin/sg2002-c906l-drm-test"
  '';
  meta = {
    description = "Standard DRM dumb-buffer and page-flip test for the C906L LCD";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "sg2002-c906l-drm-test";
  };
}
