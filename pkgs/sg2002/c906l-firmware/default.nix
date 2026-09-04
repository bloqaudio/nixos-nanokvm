{ lib
, stdenvNoCC
, fetchFromGitHub
, cmake
, ninja
, python3
, riscv64Embedded
, sg2002-c906l-rust
, contract
,
}:

let
  sourceRev = "10b86e308ca2305a464ae2bb3eb868a72295f7ab";
  knownPeripherals = [ "timer4" ];
  unknownPeripherals = lib.filter
    (peripheral: !builtins.elem peripheral knownPeripherals)
    contract.enabledPeripherals;
  enabledPeripherals = contract.enabledPeripherals;
  timer4 = builtins.elem "timer4" enabledPeripherals;
  firmwareAddress = contract.contract.memory.firmware.address;
  firmwareSize = contract.contract.memory.firmware.size;
  sharedMemoryAddress = contract.contract.memory.shared.address;
  sharedMemorySize = contract.contract.memory.shared.size;
in
assert lib.assertMsg (unknownPeripherals == [ ]) ''
  Unknown SG2002 C906L peripheral(s):
  ${lib.concatStringsSep ", " unknownPeripherals}
'';
assert lib.assertMsg
  (
    (sg2002-c906l-rust.enabledPeripherals or [ ]) == enabledPeripherals
  ) ''
  SG2002 C906L C and Rust peripheral selections must match exactly
'';
assert lib.assertMsg
  (
    (sg2002-c906l-rust.contractSha256 or null) == contract.contractSha256
      && (sg2002-c906l-rust.profileId or null) == contract.profileId
  ) ''
  SG2002 C906L C and Rust generated-contract identities must match exactly
'';
stdenvNoCC.mkDerivation (finalAttrs: {
  pname = "sg2002-c906l-firmware";
  version = "0-unstable-2024-02-07";

  src = fetchFromGitHub {
    owner = "milkv-duo";
    repo = "milkv-duo-smallcore-freertos";
    rev = sourceRev;
    hash = "sha256-3qL1+/YaAx7wTolt00qGFkSyiqBFjda+nkrZeCE4Ivk=";
  };

  sourceRoot = "${finalAttrs.src.name}/cvitek";
  strictDeps = true;
  dontConfigure = true;

  nativeBuildInputs = [
    cmake
    ninja
    python3
    riscv64Embedded.stdenv.cc
  ];

  postPatch = ''
    substituteInPlace scripts/toolchain-riscv64-elf.cmake \
      --replace-fail 'set( CMAKE_C_FLAGS "''${CMAKE_C_FLAGS} -DLINUX_BSP_64MB" )' \
                     'set( CMAKE_C_FLAGS "''${CMAKE_C_FLAGS} -DSG2002_C906L${lib.optionalString timer4 " -DSG2002_C906L_TIMER4"}" )'
    substituteInPlace build_cv181x.sh \
      --replace-fail 'cp $TOP_DIR/install/bin/cvirtos.bin ../cvirtos.bin' ':'

    # GCC's embedded libgcc is built with the low code model and cannot place
    # its __clz_tab above 2 GiB.  The generic FreeRTOS selector avoids that
    # helper and is immaterial with this firmware's two runnable tasks.
    substituteInPlace kernel/include/riscv64/FreeRTOSConfig.h \
      --replace-fail '#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1' \
                     '#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0'

    # Keep the image intentionally small: scheduler, interrupt core and the
    # mailbox/RPMsg tasks. Selected physical leases remain dormant until the
    # exact Linux acknowledgement has been validated by Rust.
    substituteInPlace driver/CMakeLists.txt \
      --replace-fail $'set(driver_list\n\tcommon\n\tpinmux\n\tuart\n\tspinlock\n\tgpio\n\trtos_cmdqu\n)' $'set(driver_list\n\tcommon\n)'
    substituteInPlace hal/cv181x/CMakeLists.txt \
      --replace-fail $'set(build_list\n\tuart\n\tpinmux\n\t# i2c\n)' 'set(build_list)'

    cp ${./rtos-shim.c} task/comm/src/riscv64/comm_main.c
    cp ${./silent-printf.c} common/src/riscv64/printf.c
    cp ${./silent-putchar.c} common/src/riscv64/putchar.c
    cp ${contract}/include/sg2002-c906l-contract.h \
      sg2002-c906l-contract.h
    cp sg2002-c906l-contract.h task/comm/include/sg2002-c906l-contract.h
    python3 ${./test_source.py} \
      task/comm/src/riscv64/comm_main.c \
      ${contract}/share/sg2002-c906l/contract.json

    # Reproducible firmware must not contain compiler wall-clock strings.
    substituteInPlace task/main/src/main.c \
      --replace-fail 'printf("CVIRTOS Build Date:%s  (Time :%s) \n", __DATE__, __TIME__);' \
                     '/* Build identity is carried by the package, not wall time. */'
    substituteInPlace task/main/src/main.c \
      --replace-fail 'static void prvSetupHardware(void);' \
                     $'static void prvSetupHardware(void);\nvoid pre_system_init(void);\nvoid post_system_init(void);'

    # Preserve the vendor's proven PLIC setup but leave every physical
    # peripheral Linux-owned during transport bring-up.
    substituteInPlace driver/common/src/system.c \
      --replace-fail $'\tpinmux_init();\n\tuart_init();\n\tirq_init();\n\tprintf("Pre system init done\\n");' \
                     $'\tirq_init();'
    substituteInPlace driver/common/src/system.c \
      --replace-fail 'void pre_system_init(void)' \
                     $'void irq_init(void);\n\nvoid pre_system_init(void)'
    substituteInPlace driver/common/src/system.c \
      --replace-fail $'\tprintf("Post system init done\\n");' ""
  '';

  buildPhase = ''
    runHook preBuild

    buildEnv="$PWD/nix-build-env"
    project=sg2002_licheervnano_c906l
    mkdir -p "$buildEnv/output/$project"
    mkdir -p install/lib
    cp ${sg2002-c906l-rust}/lib/libsg2002_c906l_rust.a install/lib/

    # The upstream build expects SDK-generated board files.  Generate their
    # deterministic equivalents directly from this package's one memory map.
    cat > "$buildEnv/.config" <<'EOF'
    CONFIG_CHIP_ARCH_cv181x=y
    CONFIG_BOARD_licheervnano=y
    CONFIG_FAST_IMAGE_TYPE=0
    CONFIG_ENABLE_FREERTOS=y
    EOF
    cp "$buildEnv/.config" milkv_duo_sd_defconfig

    cat > "$buildEnv/output/$project/cvi_board_memmap.ld" <<'EOF'
    CVIMMAP_FREERTOS_ADDR = 0x${lib.toHexString firmwareAddress};
    CVIMMAP_FREERTOS_SIZE = 0x${lib.toHexString firmwareSize};
    EOF
    cat > "$buildEnv/output/$project/cvi_board_memmap.h" <<'EOF'
    #ifndef SG2002_C906L_MEMORY_MAP_H
    #define SG2002_C906L_MEMORY_MAP_H
    #define CVIMMAP_FREERTOS_ADDR 0x${lib.toHexString firmwareAddress}
    #define CVIMMAP_FREERTOS_SIZE 0x${lib.toHexString firmwareSize}
    #endif
    EOF

    export BUILD_PATH="$buildEnv"
    export PROJECT_FULLNAME="$project"
    export CROSS_COMPILE=riscv64-none-elf-
    export SOURCE_DATE_EPOCH=1707264000
    patchShebangs build_cv181x.sh
    ./build_cv181x.sh

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    firmwareDir="$out/lib/firmware/sophgo"
    mkdir -p "$firmwareDir" "$debug/share/sg2002-c906l"
    install -m 0644 install/bin/cvirtos.bin \
      "$firmwareDir/sg2002-c906l.bin"
    install -m 0644 install/bin/cvirtos.elf \
      "$debug/share/sg2002-c906l/sg2002-c906l.elf"
    install -m 0644 install/bin/cvirtos.map \
      "$debug/share/sg2002-c906l/sg2002-c906l.map"
    install -m 0644 install/bin/cvirtos.dis \
      "$debug/share/sg2002-c906l/sg2002-c906l.dis"
    install -m 0644 sg2002-c906l-contract.h \
      "$debug/share/sg2002-c906l/contract.h"

    size=$(stat -c %s "$firmwareDir/sg2002-c906l.bin")
    test "$size" -gt 0
    test "$size" -le ${toString firmwareSize}

    # Fail the build if a toolchain/linker update moves any loadable byte out
    # of the firmware carveout or changes the RISC-V ELF class/machine.
    riscv64-none-elf-readelf -h -l \
      "$debug/share/sg2002-c906l/sg2002-c906l.elf" > elf-layout.txt
    grep -q 'Class:.*ELF64' elf-layout.txt
    grep -q 'Machine:.*RISC-V' elf-layout.txt
    grep -q 'Flags:.*double-float ABI' elf-layout.txt
    riscv64-none-elf-nm \
      "$debug/share/sg2002-c906l/sg2002-c906l.elf" \
      | grep -q ' T c906l_rust_main$'
    ${if timer4 then ''
      riscv64-none-elf-nm \
        "$debug/share/sg2002-c906l/sg2002-c906l.elf" \
        | grep -q ' T c906l_timer4_interrupt$'
    '' else ''
      if riscv64-none-elf-nm \
        "$debug/share/sg2002-c906l/sg2002-c906l.elf" \
        | grep -q ' c906l_timer4_'; then
        echo "base firmware unexpectedly contains Timer4 code" >&2
        exit 1
      fi
    ''}
    test -z "$(riscv64-none-elf-nm -u \
      "$debug/share/sg2002-c906l/sg2002-c906l.elf")"
    python3 ${./verify-elf.py} \
      "$debug/share/sg2002-c906l/sg2002-c906l.elf" \
      0x${lib.toHexString firmwareAddress} 0x${lib.toHexString firmwareSize} \
      riscv64-none-elf-readelf

    runHook postInstall
  '';

  outputs = [ "out" "debug" ];

  passthru = {
    inherit firmwareAddress firmwareSize sharedMemoryAddress sharedMemorySize;
    inherit enabledPeripherals;
    c906lContract = contract;
    inherit (contract)
      contractEpoch
      contractSha256
      dormantCapabilities
      leaseMask
      manifestFlags
      profileId
      profileName
      protocolVersion
      requiredCapabilities
      ;
    firmwareFile = "lib/firmware/sophgo/sg2002-c906l.bin";
    upstreamRev = sourceRev;
  };

  meta = {
    description = "Minimal FreeRTOS firmware for the SG2002 C906L core";
    homepage = "https://github.com/milkv-duo/milkv-duo-smallcore-freertos";
    # The FreeRTOS kernel is MIT, while Cvitek's BSP lacks sufficiently clear
    # per-file licensing.  Keep the conservative classification explicit.
    license = lib.licenses.unfreeRedistributable;
    sourceProvenance = [ lib.sourceTypes.fromSource ];
    platforms = [ "x86_64-linux" ];
  };
})
