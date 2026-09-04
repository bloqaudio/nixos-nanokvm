{
  lib,
  rustPlatform,
  rustfmt,
  stdenv,
}:

let
  canRunTests = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
in
rustPlatform.buildRustPackage {
  pname = "sg2002-c906l-ctl";
  version = "0.1.0";
  src = ../../../tools/sg2002-c906l-ctl;

  cargoLock.lockFile = ../../../tools/sg2002-c906l-ctl/Cargo.lock;
  strictDeps = true;

  doCheck = canRunTests;
  nativeCheckInputs = lib.optionals canRunTests [ rustfmt ];
  preCheck = ''
    cargo fmt --all --check
  '';

  meta = {
    description = "Bounded SG2002 C906L mailbox diagnostic and latency tool";
    license = lib.licenses.mit;
    mainProgram = "sg2002-c906l-ctl";
    platforms = lib.platforms.linux;
  };
}
