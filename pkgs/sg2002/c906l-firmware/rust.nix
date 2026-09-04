{ buildPackages
, lib
, rustPlatform
, stdenv
, contract
,
}:

let
  rustTarget = stdenv.hostPlatform.rust.rustcTarget;
  knownPeripherals = [ "timer4" ];
  unknownPeripherals = lib.filter
    (peripheral: !builtins.elem peripheral knownPeripherals)
    contract.enabledPeripherals;
  enabledPeripherals = contract.enabledPeripherals;
  timer4 = builtins.elem "timer4" enabledPeripherals;
in
assert lib.assertMsg (rustTarget == "riscv64gc-unknown-none-elf") ''
  SG2002 C906L firmware requires Rust's lp64d riscv64gc-unknown-none-elf target;
  got ${rustTarget}
'';
assert lib.assertMsg (unknownPeripherals == [ ]) ''
  Unknown SG2002 C906L Rust peripheral(s):
  ${lib.concatStringsSep ", " unknownPeripherals}
'';
rustPlatform.buildRustPackage {
  pname = "sg2002-c906l-rust";
  version = "0.1.0";
  src = ../../../firmware/sg2002-c906l;

  cargoHash = "sha256-ZpK++hvy4Cxuha7pITzEgwvFOGVUmWXjguaES8azPU4=";
  strictDeps = true;
  SG2002_C906L_CONTRACT_RS = "${contract}/rust/generated_contract.rs";

  nativeBuildInputs = [ buildPackages.python3 ];

  # The normal Cargo manifest remains an rlib so `cargo test` works on the
  # build host.  Only this target package requests the static archive consumed
  # by the vendor's proven startup/FreeRTOS link.
  buildPhase = ''
    runHook preBuild
    RUSTFLAGS="-C code-model=medium" \
      cargo build --frozen --offline --release \
        --package sg2002-c906l-static \
        ${lib.optionalString timer4 "--features timer4"} \
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
    inherit enabledPeripherals;
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
