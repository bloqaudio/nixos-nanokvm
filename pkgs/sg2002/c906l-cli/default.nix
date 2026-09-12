{ lib
, rustPlatform
, rustfmt
, stdenv
, contract
,
}:

let
  canRunTests = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  inherit (contract)
    contractEpoch
    contractSha256
    dormantCapabilities
    enabledPeripherals
    leaseMask
    manifestFlags
    profileId
    profileName
    protocolVersion
    requiredCapabilities
    ;
  contractData = contract.contract;
  firmwareAddress = contractData.memory.firmware.address;
  firmwareSize = contractData.memory.firmware.size;
  sharedMemoryAddress = contractData.memory.shared.address;
  sharedMemorySize = contractData.memory.shared.size;
  statusAddress = sharedMemoryAddress + contractData.memory.shared.regions.status.offset;
  manifestAddress = sharedMemoryAddress + contractData.activation.manifest.offset;
  activationRequestAddress = sharedMemoryAddress + contractData.activation.request.offset;
  activationRequestSize = contractData.activation.request.size;
  activationRequired = contractData.profile.activationRequired;
  capabilityWireWidth = contractData.abi.capabilityWireWidth;
  leaseWireWidth = contractData.activation.leaseWireWidth;
  manifestSize = contractData.activation.manifest.size;
  statusSize = contractData.abi.status.size;
in
assert lib.assertMsg
  (
    protocolVersion.major == 1 && protocolVersion.minor == 1
  ) "sg2002-c906l-ctl currently requires the exact ABI 1.1 contract";
assert lib.assertMsg (requiredCapabilities <= 4294967295) ''
  sg2002-c906l-ctl mailbox capability replies are u32, but the selected
  contract requires a wider value
'';
rustPlatform.buildRustPackage {
  pname = "sg2002-c906l-ctl-${profileName}";
  version = "0.1.0";
  src = ../../../tools/sg2002-c906l-ctl;

  cargoLock.lockFile = ../../../tools/sg2002-c906l-ctl/Cargo.lock;
  strictDeps = true;
  SG2002_C906L_CONTRACT_RS = "${contract}/rust/generated_contract.rs";

  doCheck = canRunTests;
  nativeCheckInputs = lib.optionals canRunTests [ rustfmt ];
  preCheck = ''
    cargo fmt --all --check
  '';

  passthru = {
    inherit
      activationRequestAddress
      activationRequestSize
      activationRequired
      capabilityWireWidth
      contract
      contractEpoch
      contractSha256
      dormantCapabilities
      enabledPeripherals
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
      requiredCapabilities
      sharedMemoryAddress
      sharedMemorySize
      statusAddress
      statusSize
      ;
    abiMajor = protocolVersion.major;
    abiMinor = protocolVersion.minor;
    c906lContract = contract;
    finalCapabilities = requiredCapabilities;
  };

  meta = {
    description = "Exact-contract SG2002 C906L mailbox and RPMsg diagnostic tool";
    license = lib.licenses.mit;
    mainProgram = "sg2002-c906l-ctl";
    platforms = lib.platforms.linux;
  };
}
