{ lib
, rustfmt
, rustPlatform
, contract
,
}:

let
  cargoFeatures = contract.cargoFeatures;
in
rustPlatform.buildRustPackage {
  pname = "sg2002-c906l-rust-tests";
  version = "0.1.0";
  src = ../../../firmware/sg2002-c906l;
  cargoHash = "sha256-69o6m4h7SPM9bPjfrJ9+bOms4pteNw/xlZwMErHT32Q=";
  CARGO_NET_OFFLINE = "true";
  nativeCheckInputs = [ rustfmt ];
  SG2002_C906L_CONTRACT_RS = "${contract}/rust/generated_contract.rs";
  cargoTestFlags = lib.optionals (cargoFeatures != [ ]) [
    "--features"
    (lib.concatStringsSep "," cargoFeatures)
  ];

  preCheck = ''
    cargo fmt --all --check
    cargo test --frozen --offline --package sg2002-pac
  '';

  # buildRustPackage runs `cargo test`; the install output is only a Hydra
  # success marker because the target archive is packaged separately.
  installPhase = ''
    runHook preInstall
    touch "$out"
    runHook postInstall
  '';

  passthru = {
    inherit cargoFeatures;
    inherit (contract) contractSha256 enabledPeripherals profileName;
  };

  meta = {
    description = "Host-side protocol and layout tests for SG2002 C906L firmware";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}
