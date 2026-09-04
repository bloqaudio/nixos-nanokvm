# USB bring-up helpers. Two binaries:
#   usb-boot          — push a FIP through the ROM, enter Sipeed
#                        vendor U-Boot's cvi_utask gadget, stage a FIT,
#                        bootm. Requires vendor FIP.
#   usb-boot-mainline — push a fastboot-only FIP through the ROM, wait
#                        for mainline U-Boot's fastboot gadget
#                        (18d1:d00d), stage a
#                        FIT via `fastboot stage` + `fastboot oem run`.
#                        Requires mainline FIP + android-tools.
{ lib
, python3
, runCommand
, writeShellApplication
, symlinkJoin
, android-tools
, sg2002-cv181x-usb-dl
, sg2002-fip
, sg2002-fip-mainline-uboot
, mainlineOnly ? false
, c906lFirmware ? sg2002-fip-mainline-uboot.rtosFirmware or null
, c906lFirmwareFile ?
    if c906lFirmware == null then null else c906lFirmware.firmwareFile or null
, c906lRunAddress ?
    if c906lFirmware == null then null
    else sg2002-fip-mainline-uboot.rtosRunAddress or null
, c906lSharedMemoryAddress ?
    if c906lFirmware == null then null
    else sg2002-fip-mainline-uboot.rtosSharedMemoryAddress or null
, c906lRequiredCapabilities ?
    if c906lFirmware == null then null
    else sg2002-fip-mainline-uboot.rtosRequiredCapabilities or null
# This is the start of U-Boot's ordinary fastboot staging buffer.  The C906L
# check runs before a FIT is staged and only reads this range, making it a
# deliberately bounded, disposable cache-eviction span.
, c906lCacheScratchAddress ?
    if c906lFirmware == null then null else 2181038080 # 0x82000000
, c906lCacheScratchSize ?
    if c906lFirmware == null then null else 1048576 # 0x00100000
, c906lReadyTimeout ? 15
,
}:
let
  pythonEnv = python3.withPackages (ps: [ ps.pyserial ps.pyusb ]);

  runnerUnitTests = runCommand "sg2002-usb-boot-mainline-unit-tests" {
    nativeBuildInputs = [ python3 ];
  } ''
    cp ${./usb_boot_mainline.py} usb_boot_mainline.py
    cp ${./test_usb_boot_mainline.py} test_usb_boot_mainline.py
    python3 -m unittest -v test_usb_boot_mainline.py
    touch "$out"
  '';

  c906lArguments =
    if c906lFirmware == null then [ ] else [
      "--c906l-firmware"
      "${c906lFirmware}/${c906lFirmwareFile}"
      "--c906l-run-address"
      "0x${lib.toHexString c906lRunAddress}"
      "--c906l-shmem-address"
      "0x${lib.toHexString c906lSharedMemoryAddress}"
      "--c906l-required-capabilities"
      "0x${lib.toHexString c906lRequiredCapabilities}"
      "--c906l-cache-scratch-address"
      "0x${lib.toHexString c906lCacheScratchAddress}"
      "--c906l-cache-scratch-size"
      "0x${lib.toHexString c906lCacheScratchSize}"
      "--c906l-ready-timeout"
      (toString c906lReadyTimeout)
    ];

  # Path to the upstream cv181x-rom-dl's lib dir — contains the
  # `cv_usb_util` package + `cv_dl_magic.bin`.
  cvUsbLib = "${sg2002-cv181x-usb-dl}/lib/cv181x-usb-dl/rom_usb_dl";

  # fast-rom-dl used to live here as a libusb-only replacement for
  # the FIP-push phase. Dropped because it only handled the 1st-stage
  # push (magic + first 4 KB + BREAK); the
  # vendor FSBL we still link against needs a 2nd-stage cvi_utask
  # transfer to receive the rest of FIP. Without a libusb impl of
  # 2nd-stage it was an incomplete shortcut that just clutters the
  # output. See ./fast_rom_dl.py history for the partial impl if
  # someone wants to finish it; meanwhile, both `usb-boot` runners
  # below shell out to upstream `cv181x-rom-dl` (with our pyserial
  # timeout / fast-open patches; see pkgs/sg2002-cv181x-rom-dl-…).

  usb-boot-vendor = writeShellApplication {
    name = "usb-boot";
    text = ''
      exec ${pythonEnv}/bin/python3 ${./usb_boot.py} \
        --rom-dl ${sg2002-cv181x-usb-dl}/bin/cv181x-rom-dl \
        --cv-usb-lib ${cvUsbLib} \
        --fip ${sg2002-fip} \
        "$@"
    '';
  };

  usb-boot-mainline = writeShellApplication {
    name = "usb-boot-mainline";
    runtimeInputs = [ android-tools ];
    text = ''
      exec ${pythonEnv}/bin/python3 ${./usb_boot_mainline.py} \
        --rom-dl ${sg2002-cv181x-usb-dl}/bin/cv181x-rom-dl \
        --fip ${sg2002-fip-mainline-uboot} \
        ${lib.escapeShellArgs c906lArguments} \
        "$@"
    '';
  };
in
assert lib.assertMsg (
  if c906lFirmware == null then
    c906lFirmwareFile == null
    && c906lRunAddress == null
    && c906lSharedMemoryAddress == null
    && c906lRequiredCapabilities == null
    && c906lCacheScratchAddress == null
    && c906lCacheScratchSize == null
  else
    c906lFirmwareFile != null
    && c906lRunAddress != null
    && c906lSharedMemoryAddress != null
    && c906lRequiredCapabilities != null
    && c906lCacheScratchAddress != null
    && c906lCacheScratchSize != null
) ''
  A C906L USB runner requires a firmware file, run address, shared-memory
  address, required capability mask, and explicitly safe cache-scratch
  address/size
'';
if mainlineOnly then
  symlinkJoin {
    name = "sg2002-usb-boot-mainline";
    paths = [ usb-boot-mainline ];
    postBuild = ''
      # A package parameterised by a mainline FIP must not leave the generic
      # `usb-boot` name pointing at the unrelated vendor FIP.  Retain the
      # explicit name for artifact builders and make the ordinary entry point
      # an alias of exactly the same, caller-supplied mainline runner.
      ln -s usb-boot-mainline $out/bin/usb-boot
    '';
    passthru.tests.runner = runnerUnitTests;
    meta.mainProgram = "usb-boot";
  }
else
  symlinkJoin {
    name = "sg2002-usb-boot";
    paths = [ usb-boot-vendor usb-boot-mainline ];
    passthru.tests.mainlineRunner = runnerUnitTests;
  }
