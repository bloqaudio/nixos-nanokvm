{ lib
, rustfmt
, rustPlatform
, contract
,
}:

let
  knownPeripherals = [ "timer4" ];
  unknownPeripherals = lib.filter
    (peripheral: !builtins.elem peripheral knownPeripherals)
    contract.enabledPeripherals;
  timer4 = builtins.elem "timer4" contract.enabledPeripherals;
in
assert lib.assertMsg (unknownPeripherals == [ ]) ''
  Unknown SG2002 C906L Rust test peripheral(s):
  ${lib.concatStringsSep ", " unknownPeripherals}
'';
rustPlatform.buildRustPackage {
  pname = "sg2002-c906l-rust-tests";
  version = "0.1.0";
  src = ../../../firmware/sg2002-c906l;
  cargoHash = "sha256-ZpK++hvy4Cxuha7pITzEgwvFOGVUmWXjguaES8azPU4=";
  nativeCheckInputs = [ rustfmt ];
  SG2002_C906L_CONTRACT_RS = "${contract}/rust/generated_contract.rs";
  cargoTestFlags = lib.optionals timer4 [ "--features" "timer4" ];

  preCheck = ''
    cargo fmt --all --check
  '';

  # buildRustPackage runs `cargo test`; the install output is only a Hydra
  # success marker because the target archive is packaged separately.
  installPhase = ''
    runHook preInstall
    touch "$out"
    runHook postInstall
  '';

  passthru = {
    inherit (contract) contractSha256 enabledPeripherals profileName;
  };

  meta = {
    description = "Host-side protocol and layout tests for SG2002 C906L firmware";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}
