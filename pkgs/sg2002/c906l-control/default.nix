{
  lib,
  stdenv,
  kernel,
}:

stdenv.mkDerivation {
  pname = "sg2002-c906l-control";
  version = "0.1.0-${kernel.modDirVersion}";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./Makefile
      ./sg2002-c906l-control.c
    ];
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" "format" ];

  makeFlags = [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "ARCH=riscv"
    "CROSS_COMPILE=${stdenv.cc.targetPrefix}"
  ];

  installPhase = ''
    runHook preInstall
    install -Dm0644 sg2002-c906l-control.ko \
      "$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/misc/sg2002-c906l-control.ko"
    runHook postInstall
  '';

  meta = {
    description = "Linux mailbox control endpoint for the SG2002 C906L";
    license = lib.licenses.gpl2Only;
    platforms = [ "riscv64-linux" ];
  };
}
