{
  lib,
  stdenvNoCC,
  makeWrapper,
  python3,
  src,
}:

stdenvNoCC.mkDerivation {
  pname = "sg2002-fiptool";
  version = "0-unstable-2024-12-28";
  inherit src;

  patches = [ ./explicit-rtos-runaddr.patch ];
  nativeBuildInputs = [ makeWrapper python3 ];
  dontConfigure = true;
  dontBuild = true;

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    python3 -m py_compile fiptool
    if python3 fiptool --rtos data/cvirtos.bin ignored.bin 2>error.log; then
      echo "fiptool accepted RTOS firmware without an execution address" >&2
      exit 1
    fi
    grep -q -- '--rtos requires a non-zero --rtos-runaddr' error.log
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/libexec/sg2002-fiptool" "$out/bin"
    cp -r data "$out/libexec/sg2002-fiptool/"
    install -m 0555 fiptool "$out/libexec/sg2002-fiptool/fiptool"
    makeWrapper ${lib.getExe python3} "$out/bin/sg2002-fiptool" \
      --add-flags "$out/libexec/sg2002-fiptool/fiptool"
    runHook postInstall
  '';

  meta = {
    description = "Sophgo FIP packer with explicit, fail-safe C906L loading";
    homepage = "https://github.com/sophgo/fiptool";
    license = lib.licenses.bsd2;
    mainProgram = "sg2002-fiptool";
    platforms = lib.platforms.all;
  };
}
