{
  buildPackages,
  lib,
  runCommand,
  profile ? "base",
  peripherals ? null,
}:

let
  contractLib = import ./lib.nix { inherit lib; };
  libraryTests = import ./test-lib.nix { inherit lib; };
  selected =
    if peripherals == null then
      contractLib.resolveProfile profile
    else
      contractLib.resolvePeripherals peripherals;
  profiles = contractLib.profiles;
  profileNames = builtins.attrNames profiles;
  profileFiles = lib.mapAttrs
    (name: resolved:
      builtins.toFile "sg2002-c906l-contract-${name}.json"
        resolved.canonicalJson)
    profiles;
  profileTestArguments = lib.concatMapStringsSep " "
    (name: "--profile ${lib.escapeShellArg name} ${profileFiles.${name}} ${profiles.${name}.sha256}")
    profileNames;
  compileProfiles = lib.concatMapStringsSep "\n"
    (name: ''
      generated="$TMPDIR/generated-${name}"
      python3 ${./generate.py} \
        --contract ${profileFiles.${name}} \
        --sha256 ${profiles.${name}.sha256} \
        --output "$generated"
      cc -std=c11 -Wall -Wextra -Werror \
        -I "$generated/include" -c ${./compile-test.c} \
        -o "$TMPDIR/${name}-contract.o"
      rustc --edition 2024 --crate-type lib \
        "$generated/rust/generated_contract.rs" \
        -o "$TMPDIR/lib${name}-contract.rlib"
      cpp -nostdinc -undef -x assembler-with-cpp \
        -DIRQ_TYPE_LEVEL_HIGH=4 -I "$generated/dts" \
        ${./compile-test.dts} "$TMPDIR/${name}.dts"
      dtc -I dts -O dtb -o "$TMPDIR/${name}.dtb" \
        "$TMPDIR/${name}.dts"
    '')
    profileNames;

  generatorTests = runCommand "sg2002-c906l-contract-tests" {
    nativeBuildInputs = [
      buildPackages.dtc
      buildPackages.python3
      buildPackages.rustc
      buildPackages.stdenv.cc
    ];
  } ''
    python3 ${./test_generate.py} \
      --generator ${./generate.py} \
      ${profileTestArguments}

    ${compileProfiles}
    touch "$out"
  '';
in
assert builtins.deepSeq libraryTests true;
runCommand "sg2002-c906l-contract-${selected.profileName}" {
  nativeBuildInputs = [ buildPackages.python3 ];
  passAsFile = [ "resolvedContract" ];
  resolvedContract = selected.canonicalJson;

  passthru = {
    contract = selected.resolvedContract;
    contractEpoch = selected.resolvedContract.contractEpoch;
    contractSha256 = selected.sha256;
    dormantCapabilities = selected.dormantCapabilities;
    enabledPeripherals = selected.sortedPeripherals;
    leaseMask = selected.leaseMask;
    manifestFlags = selected.manifestFlags;
    profileName = selected.profileName;
    profileId = selected.profileId;
    protocolVersion = {
      inherit (selected.resolvedContract.abi) major minor;
    };
    requiredCapabilities = selected.expectedCapabilities;
    tests = {
      generator = generatorTests;
    };
  };

  meta = {
    description = "Generated ${selected.profileName} contract for the SG2002 C906L";
    license = lib.licenses.mit;
    platforms = lib.platforms.all;
  };
} ''
  python3 ${./generate.py} \
    --contract "$resolvedContractPath" \
    --sha256 ${selected.sha256} \
    --output "$out"
''
