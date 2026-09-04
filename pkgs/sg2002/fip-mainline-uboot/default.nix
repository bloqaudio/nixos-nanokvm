# Fully-mainline FIP: vendor FSBL + vendor DDR params (both extracted
# from the known-working vendor fip.bin, since sophgo's own prebuilt
# `data/fsbl/cv181x.bin` won't POST on the LicheeRV Nano — it drops
# into an eMMC retry loop) + mainline OpenSBI 1.8.1 (with embedded
# U-Boot DTB so it has an FDT even if the FSBL doesn't pass one via
# fw_dynamic_info) + mainline U-Boot 2026.04, wrapped by sophgo/fiptool
# (LZMA-compressed B3MA blob; Sipeed's fiptool.py BL33-magic repack
# path produces a blob that doesn't decompress on-target).
{
  lib,
  runCommand,
  python3,
  sg2002-fip,
  sg2002-fiptool,
  sg2002-opensbi-mainline,
  sg2002-uboot-mainline,
  rtosFirmware ? null,
}:
let
  memoryMap = import ../c906l-memory-map.nix;
  haveRtos = rtosFirmware != null;
  contractFields = [
    "firmwareAddress"
    "firmwareFile"
    "firmwareSize"
    "requiredCapabilities"
    "sharedMemoryAddress"
    "sharedMemorySize"
  ];
  rtosHasContract = !haveRtos
    || lib.all (name: builtins.hasAttr name rtosFirmware) contractFields;
  rtosValue = name: fallback:
    if haveRtos && rtosHasContract then builtins.getAttr name rtosFirmware else fallback;
  rtosRunAddress = rtosValue "firmwareAddress" 0;
  rtosFirmwareSize = rtosValue "firmwareSize" 0;
  rtosSharedMemoryAddress =
    rtosValue "sharedMemoryAddress" 0;
  rtosSharedMemorySize = rtosValue "sharedMemorySize" 0;
  rtosRequiredCapabilities = rtosValue "requiredCapabilities" 0;
  rtosFile =
    if haveRtos && rtosHasContract
    then "${rtosFirmware}/${rtosFirmware.firmwareFile}"
    else null;
  inherit (memoryMap) dramEnd;
  requiredMemoryTopHide =
    if haveRtos then dramEnd - rtosRunAddress else 0;
  ubootMemoryTopHide = sg2002-uboot-mainline.memoryTopHide or 0;
in
assert lib.assertMsg rtosHasContract ''
  SG2002 RTOS package must expose ${lib.concatStringsSep ", " contractFields}
'';
assert lib.assertMsg (!haveRtos || (
  rtosRunAddress > 0
  && rtosFirmwareSize > 0
  && rtosSharedMemorySize > 0
  && rtosRunAddress + rtosFirmwareSize == rtosSharedMemoryAddress
  && rtosSharedMemoryAddress + rtosSharedMemorySize == dramEnd
)) ''
  SG2002 C906L firmware and shared memory must form one contiguous top-of-DRAM
  reservation ending at 0x${lib.toHexString dramEnd}
'';
assert lib.assertMsg (!haveRtos || ubootMemoryTopHide >= requiredMemoryTopHide) ''
  SG2002 C906L FIP needs U-Boot memoryTopHide >=
  0x${lib.toHexString requiredMemoryTopHide}; got
  0x${lib.toHexString ubootMemoryTopHide}.  Without it U-Boot can clear the
  executing firmware while initializing its top-of-RAM malloc arena.
'';
runCommand "fip-sg2002-mainline-uboot${if haveRtos then "-c906l" else ""}" {
  nativeBuildInputs = [python3];
  passthru = {
    inherit
      rtosFirmware
      rtosRunAddress
      rtosFirmwareSize
      rtosSharedMemoryAddress
      rtosSharedMemorySize
      rtosRequiredCapabilities
      ;
  };
} ''
  mkdir -p $out workdir

  python3 ${./extract-vendor-bits.py} ${sg2002-fip}/fip.bin workdir

  ${sg2002-fiptool}/bin/sg2002-fiptool \
    --fsbl      workdir/vendor-fsbl.bin \
    --ddr_param workdir/vendor-ddr.bin \
    ${if haveRtos then "--rtos ${rtosFile} --rtos-runaddr 0x${lib.toHexString rtosRunAddress}" else ""} \
    --opensbi   ${sg2002-opensbi-mainline}/share/opensbi/lp64/generic/firmware/fw_dynamic.bin \
    --uboot     ${sg2002-uboot-mainline}/u-boot.bin \
    $out/fip.bin

  python3 ${./verify-fip.py} $out/fip.bin \
    ${if haveRtos then "--rtos ${rtosFile} --rtos-runaddr 0x${lib.toHexString rtosRunAddress}" else ""}
''
