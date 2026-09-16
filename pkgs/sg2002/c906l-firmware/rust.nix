{ buildPackages
, lib
, rustPlatform
, stdenv
, contract
,
}:

let
  rustTarget = stdenv.hostPlatform.rust.rustcTarget;
  enabledPeripherals = contract.enabledPeripherals;
  cargoFeatures = contract.cargoFeatures;
  cargoFeatureFlags = lib.optionalString (cargoFeatures != [ ])
    "--features ${lib.escapeShellArg (lib.concatStringsSep "," cargoFeatures)}";
in
assert lib.assertMsg (rustTarget == "riscv64gc-unknown-none-elf") ''
  SG2002 C906L firmware requires Rust's lp64d riscv64gc-unknown-none-elf target;
  got ${rustTarget}
'';
rustPlatform.buildRustPackage {
  pname = "sg2002-c906l-rust";
  version = "0.1.0";
  src = ../../../firmware/sg2002-c906l;

  cargoHash = "sha256-cuRtDH2k7bJNt6llvkwXnnDoOwWrGQwP+Li4Uw0WN1k=";
  strictDeps = true;
  CARGO_NET_OFFLINE = "true";
  SG2002_C906L_CONTRACT_RS = "${contract}/rust/generated_contract.rs";

  # safe-mmio's zerocopy dependency uses proc-macro/build-script crates, so
  # Cargo needs a native linker in addition to the bare-metal target tools.
  nativeBuildInputs = [
    buildPackages.python3
    buildPackages.stdenv.cc
  ];

  # The normal Cargo manifest remains an rlib so `cargo test` works on the
  # build host.  Only this target package requests the static archive consumed
  # by the vendor's proven startup/FreeRTOS link.
  buildPhase = ''
    runHook preBuild
    RUSTFLAGS="-C code-model=medium" \
      cargo build --frozen --offline --release \
        --package sg2002-c906l-static \
        ${cargoFeatureFlags} \
        --target ${rustTarget}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/lib"
    install -m 0644 \
      target/${rustTarget}/release/libsg2002_c906l_static.a \
      "$out/lib/libsg2002_c906l_rust.a"
    python3 ${./verify-staticlib.py} \
      "$out/lib/libsg2002_c906l_rust.a" \
      ${stdenv.cc.targetPrefix}ar ${stdenv.cc.targetPrefix}readelf \
      ${rustPlatform.rust.rustc}/bin/rustc ${rustTarget}
    ${stdenv.cc.targetPrefix}nm "$out/lib/libsg2002_c906l_rust.a" \
      | grep -q ' T c906l_rust_main$'
    runHook postInstall
  '';

  doCheck = false; # Host-side tests are a separate native derivation/check.

  passthru = {
    inherit cargoFeatures enabledPeripherals;
    inherit (contract)
      contractEpoch
      contractSha256
      dormantCapabilities
      leaseMask
      profileId
      profileName
      protocolVersion
      requiredCapabilities
      ;
  };

  meta = {
    description = "Rust application layer for the SG2002 C906L firmware";
    license = lib.licenses.mit;
    sourceProvenance = [ lib.sourceTypes.fromSource ];
    platforms = [ "riscv64-none" ];
  };
}
