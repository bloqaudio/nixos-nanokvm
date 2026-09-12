# Fully-mainline FIP: vendor FSBL + vendor DDR params (both extracted
# from the known-working vendor fip.bin, since sophgo's own prebuilt
# `data/fsbl/cv181x.bin` won't POST on the LicheeRV Nano — it drops
# into an eMMC retry loop) + mainline OpenSBI 1.8.1 (with embedded
# U-Boot DTB so it has an FDT even if the FSBL doesn't pass one via
# fw_dynamic_info) + mainline U-Boot 2026.04, wrapped by sophgo/fiptool
# (LZMA-compressed B3MA blob; Sipeed's fiptool.py BL33-magic repack
# path produces a blob that doesn't decompress on-target).
{ lib
, runCommand
, python3
, sg2002-fip
, sg2002-fiptool
, sg2002-opensbi-mainline
, sg2002-uboot-mainline
, rtosFirmware ? null
, c906lContract ? if rtosFirmware == null then null else rtosFirmware.c906lContract or null
,
}:
let
  memoryMap = import ../c906l-memory-map.nix { inherit lib; };
  haveRtos = rtosFirmware != null;
  haveContract = c906lContract != null;
  firmwareContractFields = [
    "contractEpoch"
    "contractSha256"
    "dormantCapabilities"
    "enabledPeripherals"
    "firmwareAddress"
    "firmwareFile"
    "firmwareSize"
    "leaseMask"
    "manifestFlags"
    "profileId"
    "profileName"
    "protocolVersion"
    "requiredCapabilities"
    "sharedMemoryAddress"
    "sharedMemorySize"
  ];
  rtosHasContract = !haveRtos
    || lib.all (name: builtins.hasAttr name rtosFirmware) firmwareContractFields;
  contract = if haveContract then c906lContract.contract else null;
  firmwareAddress = if haveContract then contract.memory.firmware.address else 0;
  firmwareSize = if haveContract then contract.memory.firmware.size else 0;
  sharedMemoryAddress = if haveContract then contract.memory.shared.address else 0;
  sharedMemorySize = if haveContract then contract.memory.shared.size else 0;
  statusAddress =
    if haveContract then
      sharedMemoryAddress + contract.memory.shared.regions.status.offset
    else 0;
  statusSize = if haveContract then contract.abi.status.size else 0;
  manifestAddress =
    if haveContract then
      sharedMemoryAddress + contract.activation.manifest.offset
    else 0;
  manifestSize = if haveContract then contract.activation.manifest.size else 0;
  activationRequestAddress =
    if haveContract then
      sharedMemoryAddress + contract.activation.request.offset
    else 0;
  activationRequestSize = if haveContract then contract.activation.request.size else 0;
  contractEpoch = if haveContract then c906lContract.contractEpoch else 0;
  contractSha256 = if haveContract then c906lContract.contractSha256 else null;
  protocolVersion = if haveContract then c906lContract.protocolVersion else null;
  abiMajor = if haveContract then protocolVersion.major else 0;
  abiMinor = if haveContract then protocolVersion.minor else 0;
  profileId = if haveContract then c906lContract.profileId else 0;
  profileName = if haveContract then c906lContract.profileName else null;
  finalCapabilities = if haveContract then c906lContract.requiredCapabilities else 0;
  dormantCapabilities = if haveContract then c906lContract.dormantCapabilities else 0;
  leaseMask = if haveContract then c906lContract.leaseMask else 0;
  manifestFlags = if haveContract then c906lContract.manifestFlags else 0;
  enabledPeripherals = if haveContract then c906lContract.enabledPeripherals else [ ];
  activationRequired = if haveContract then contract.profile.activationRequired else false;
  capabilityWireWidth = if haveContract then contract.abi.capabilityWireWidth else 0;
  leaseWireWidth = if haveContract then contract.activation.leaseWireWidth else 0;
  firmwareMatchesContract = !haveRtos || !rtosHasContract || (
    rtosFirmware.contractEpoch == contractEpoch
      && rtosFirmware.contractSha256 == contractSha256
      && rtosFirmware.dormantCapabilities == dormantCapabilities
      && rtosFirmware.enabledPeripherals == enabledPeripherals
      && rtosFirmware.firmwareAddress == firmwareAddress
      && rtosFirmware.firmwareSize == firmwareSize
      && rtosFirmware.leaseMask == leaseMask
      && rtosFirmware.manifestFlags == manifestFlags
      && rtosFirmware.profileId == profileId
      && rtosFirmware.profileName == profileName
      && rtosFirmware.protocolVersion == protocolVersion
      && rtosFirmware.requiredCapabilities == finalCapabilities
      && rtosFirmware.sharedMemoryAddress == sharedMemoryAddress
      && rtosFirmware.sharedMemorySize == sharedMemorySize
  );
  rtosRunAddress = firmwareAddress;
  rtosFirmwareSize = firmwareSize;
  rtosSharedMemoryAddress = sharedMemoryAddress;
  rtosSharedMemorySize = sharedMemorySize;
  rtosRequiredCapabilities = finalCapabilities;
  rtosFile =
    if haveRtos && rtosHasContract
    then "${rtosFirmware}/${rtosFirmware.firmwareFile}"
    else null;
  inherit (memoryMap) dramEnd;
  requiredMemoryTopHide =
    if haveRtos then dramEnd - rtosRunAddress else 0;
  ubootMemoryTopHide = sg2002-uboot-mainline.memoryTopHide or 0;
in
assert lib.assertMsg (haveRtos == haveContract) ''
  SG2002 C906L FIP packaging requires rtosFirmware and its profile-specific
  c906lContract package to be supplied together
'';
assert lib.assertMsg rtosHasContract ''
  SG2002 RTOS package must expose
  ${lib.concatStringsSep ", " firmwareContractFields}
'';
assert lib.assertMsg firmwareMatchesContract ''
  SG2002 RTOS firmware metadata does not exactly match its selected canonical
  C906L contract package; refusing to produce a misidentified FIP
'';
assert lib.assertMsg (!haveRtos || (abiMajor == 1 && abiMinor == 1)) ''
  SG2002 C906L FIP packaging currently requires the exact ABI 1.1 contract
'';
assert lib.assertMsg
  (!haveRtos || (
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
runCommand "fip-sg2002-mainline-uboot${if haveRtos then "-c906l-${profileName}" else ""}"
{
  nativeBuildInputs = [ python3 ];
  passthru = {
    inherit
      rtosFirmware
      rtosRunAddress
      rtosFirmwareSize
      rtosSharedMemoryAddress
      rtosSharedMemorySize
      rtosRequiredCapabilities
      ;
  } // lib.optionalAttrs haveRtos {
    inherit
      abiMajor
      abiMinor
      activationRequestAddress
      activationRequestSize
      activationRequired
      capabilityWireWidth
      c906lContract
      contractEpoch
      contractSha256
      dormantCapabilities
      enabledPeripherals
      finalCapabilities
      firmwareAddress
      firmwareSize
      leaseMask
      leaseWireWidth
      manifestAddress
      manifestFlags
      manifestSize
      profileId
      profileName
      protocolVersion
      sharedMemoryAddress
      sharedMemorySize
      statusAddress
      statusSize
      ;
    requiredCapabilities = finalCapabilities;
    c906lIdentity = {
      inherit
        abiMajor
        abiMinor
        activationRequestAddress
        activationRequestSize
        activationRequired
        capabilityWireWidth
        contractEpoch
        contractSha256
        dormantCapabilities
        enabledPeripherals
        finalCapabilities
        firmwareAddress
        firmwareSize
        leaseMask
        leaseWireWidth
        manifestAddress
        manifestFlags
        manifestSize
        profileId
        profileName
        protocolVersion
        sharedMemoryAddress
        sharedMemorySize
        statusAddress
        statusSize
        ;
    };
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
    ${if haveRtos then "--rtos ${rtosFile} --rtos-runaddr 0x${lib.toHexString rtosRunAddress} --rtos-contract-sha256 ${contractSha256}" else ""}
''
