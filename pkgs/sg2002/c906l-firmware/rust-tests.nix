{
  lib,
  rustfmt,
  rustPlatform,
}:

let
  memoryMap = import ../c906l-memory-map.nix { inherit lib; };
in
rustPlatform.buildRustPackage {
  pname = "sg2002-c906l-rust-tests";
  version = "0.1.0";
  src = ../../../firmware/sg2002-c906l;
  cargoHash = "sha256-ZpK++hvy4Cxuha7pITzEgwvFOGVUmWXjguaES8azPU4=";
  nativeCheckInputs = [ rustfmt ];

  postPatch = ''
    substituteInPlace src/lib.rs \
      --replace-fail \
        'const SHMEM_BASE: usize = 0x8ff0_0000;' \
        'const SHMEM_BASE: usize = 0x${lib.toHexString memoryMap.sharedMemoryAddress};'
  '';

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

  meta = {
    description = "Host-side protocol and layout tests for SG2002 C906L firmware";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}
