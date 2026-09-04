{
  lib,
  stdenv,
  kernel,
}:

let
  memoryMap = import ../c906l-memory-map.nix { inherit lib; };
  hex = value: "0x${lib.toHexString value}ULL";
in
assert lib.assertMsg (
  memoryMap.resourceTableAddress == memoryMap.sharedMemoryAddress + 4096
  && memoryMap.rpmsgVring0Address == memoryMap.sharedMemoryAddress + 8192
  && memoryMap.rpmsgVring1Address == memoryMap.sharedMemoryAddress + 24576
  && memoryMap.rpmsgBufferAddress == memoryMap.sharedMemoryAddress + 65536
  && memoryMap.bulkAddress == memoryMap.sharedMemoryAddress + 327680
  && memoryMap.bulkAddress + memoryMap.bulkSize
    == memoryMap.sharedMemoryAddress + memoryMap.sharedMemorySize
) "SG2002 C906L RPMsg subregions must exactly partition the shared carveout";
stdenv.mkDerivation {
  pname = "sg2002-c906l-remoteproc";
  version = "0.1.0-${kernel.modDirVersion}";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./Makefile
      ./sg2002-c906l-remoteproc.c
    ];
  };

  postPatch = ''
    substituteInPlace sg2002-c906l-remoteproc.c \
      --replace-fail '@SHMEM_ADDRESS@' '${hex memoryMap.sharedMemoryAddress}' \
      --replace-fail '@SHMEM_SIZE@' '${hex memoryMap.sharedMemorySize}' \
      --replace-fail '@RESOURCE_TABLE_OFFSET@' '${hex (memoryMap.resourceTableAddress - memoryMap.sharedMemoryAddress)}' \
      --replace-fail '@RESOURCE_TABLE_SIZE@' '${hex memoryMap.resourceTableSize}' \
      --replace-fail '@VRING0_OFFSET@' '${hex (memoryMap.rpmsgVring0Address - memoryMap.sharedMemoryAddress)}' \
      --replace-fail '@VRING0_SIZE@' '${hex memoryMap.rpmsgVring0Size}' \
      --replace-fail '@VRING1_OFFSET@' '${hex (memoryMap.rpmsgVring1Address - memoryMap.sharedMemoryAddress)}' \
      --replace-fail '@VRING1_SIZE@' '${hex memoryMap.rpmsgVring1Size}' \
      --replace-fail '@BUFFER_OFFSET@' '${hex (memoryMap.rpmsgBufferAddress - memoryMap.sharedMemoryAddress)}' \
      --replace-fail '@BUFFER_SIZE@' '${hex memoryMap.rpmsgBufferSize}'
  '';

  nativeBuildInputs = kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" "format" ];

  makeFlags = [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "ARCH=riscv"
    "CROSS_COMPILE=${stdenv.cc.targetPrefix}"
  ];

  installPhase = ''
    runHook preInstall
    install -Dm0644 sg2002-c906l-remoteproc.ko \
      "$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/remoteproc/sg2002-c906l-remoteproc.ko"
    runHook postInstall
  '';

  meta = {
    description = "Attach-only Linux remoteproc/RPMsg transport for SG2002 C906L";
    license = lib.licenses.gpl2Only;
    platforms = [ "riscv64-linux" ];
  };
}
