{
  lib,
  makeRustPlatform,
  rustPlatform,
  riscv64Embedded,
  wrapRustcWith,
}:

let
  rustTarget = riscv64Embedded.stdenv.hostPlatform.rust.rustcTarget;
  expectedRustTarget = "riscv64gc-unknown-none-elf";

  # compiler_builtins uses cc-rs for its optimized compiler-rt routines.
  # cc-rs defaults every bare-metal rv64 target to lp64, including Rust's
  # lp64d riscv64gc target, so the stock sysroot is internally ABI-mixed.
  # Target-specific CFLAGS are appended after cc-rs' defaults and make the
  # entire sysroot agree with Rust's built-in target specification.
  targetCFlags = "-march=rv64gc -mabi=lp64d -mcmodel=medany";
  targetCFlagsName = "CFLAGS_${lib.replaceStrings [ "-" ] [ "_" ] rustTarget}";

  rustcUnwrapped =
    riscv64Embedded.rustPlatform.rust.rustc.unwrapped.overrideAttrs
      (old: {
        # rustc enables structured attributes.  Values that subprocesses such
        # as compiler_builtins' build script must see belong in env, rather
        # than as ordinary derivation attributes.
        env = (old.env or { }) // {
          "${targetCFlagsName}" = targetCFlags;
        };
      });

  rustc = wrapRustcWith {
    rustc-unwrapped = rustcUnwrapped;
    # fastCross builds only the target libraries.  Point the host rustc binary
    # at those rebuilt libraries instead of the stock, ABI-mixed sysroot.
    sysroot = rustcUnwrapped;
  };
in
assert lib.assertMsg (rustTarget == expectedRustTarget) ''
  SG2002 C906L Rust packaging expected ${expectedRustTarget}, got ${rustTarget};
  audit the target ABI and compiler_builtins C flags before changing it
'';
makeRustPlatform {
  inherit rustc;
  stdenv = riscv64Embedded.stdenv;

  # The cross Cargo wrapper prepends its original rustc to PATH, bypassing the
  # corrected sysroot above.  Native Cargo is target-neutral and the Rust
  # platform adds our explicit rustc to the build environment ahead of it.
  cargo = rustPlatform.rust.cargo;
}
