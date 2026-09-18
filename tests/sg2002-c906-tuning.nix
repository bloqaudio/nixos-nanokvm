{ pkgs, targetPkgs }:
let
  lib = pkgs.lib;
  cc = targetPkgs.stdenv.cc;
  libcCC = targetPkgs.glibc.stdenv.cc;
  nativeCC = targetPkgs.buildPackages.stdenv.cc;
  kernel = targetPkgs.sg2002-kernel-mainline;
  genericKernel = pkgs.pkgsCross.riscv64.sg2002-kernel-mainline;
  kernelSource = pkgs.writeText "c906-tuning-kernel.c" ''
    #include <linux/types.h>
    #if !defined(__riscv) || __riscv_xlen != 64
    #error wrong target
    #endif
    #if defined(__riscv_vector) || defined(__riscv_xtheadvector)
    #error tuning must not enable vector instructions
    #endif
    u32 c906_tuning_probe(u32 value);
    u32 c906_tuning_probe(u32 value) { return value * 31 + 7; }
  '';
  kernelMakefile = pkgs.writeText "c906-tuning-kernel-Makefile" ''
    obj-m := probe.o
  '';
  testSource = pkgs.writeText "c906-tuning-test.c" ''
    #include <stdio.h>
    #if !defined(__riscv) || __riscv_xlen != 64
    #error wrong target
    #endif
    #if defined(__riscv_vector) || defined(__riscv_xtheadvector)
    #error tuning must not enable vector instructions
    #endif
    int main(void) { puts("c906-tuning-ok"); return 0; }
  '';
in
assert targetPkgs.stdenv.hostPlatform.gcc.tune == "thead-c906";
assert lib.hasInfix "-mtune=thead-c906" cc.drvAttrs.postFixup;
assert lib.hasInfix "-mtune=thead-c906" libcCC.drvAttrs.postFixup;
assert !(lib.hasInfix "-mtune=thead-c906" nativeCC.drvAttrs.postFixup);
assert builtins.elem "KCFLAGS=-mtune=thead-c906" kernel.commonMakeFlags;
assert builtins.elem "KCFLAGS=-mtune=thead-c906" kernel.makeFlags;
assert !(lib.any (lib.hasPrefix "KCFLAGS=-mtune=") genericKernel.makeFlags);
pkgs.runCommand "sg2002-c906-tuning-tests" {
  nativeBuildInputs = [ cc pkgs.qemu pkgs.gnugrep ] ++ kernel.moduleBuildDependencies;
} ''
  # Inspect both the final target compiler and libc's bootstrap compiler.
  for compiler in ${cc} ${libcCC}; do
    test "$(grep -Fo -- '-mtune=thead-c906' "$compiler/nix-support/cc-cflags-before" | wc -l)" = 1
  done
  ! grep -q -- '-mtune=thead-c906' ${nativeCC}/nix-support/cc-cflags-before

  # GCC's own report proves the wrapper flag reached the compiler, rather
  # than merely being present in derivation metadata.
  ${cc.targetPrefix}gcc -Q --help=target -c -x c /dev/null -o probe.o > defaults
  grep -E -- '-mtune=[^[:space:]]*[[:space:]]+thead-c906$' defaults
  ${cc.targetPrefix}gcc -mtune=rocket -Q --help=target -c -x c /dev/null -o probe.o > override
  grep -E -- '-mtune=[^[:space:]]*[[:space:]]+rocket$' override
  ${cc.targetPrefix}gcc -O2 ${testSource} -o test-c
  ${cc.targetPrefix}g++ -O2 -x c++ ${testSource} -o test-cxx
  for binary in test-c test-cxx; do
    qemu-riscv64 -cpu thead-c906 "$binary" | grep -Fx c906-tuning-ok
  done

  # Exercise Kbuild with the actual kernel's unwrapped compiler and common
  # make flags. Checking cc-wrapper alone misses the kernel entirely.
  cp -r ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build kernel-build
  chmod -R u+w kernel-build
  mkdir kernel-probe
  cp ${kernelSource} kernel-probe/probe.c
  cp ${kernelMakefile} kernel-probe/Makefile
  make -C kernel-build ${lib.escapeShellArgs kernel.commonMakeFlags} \
    O="$PWD/kernel-build" M="$PWD/kernel-probe" probe.o
  grep -F -- '${lib.getExe kernel.stdenv.cc.cc}' kernel-probe/.probe.o.cmd
  grep -F -- '-mtune=thead-c906' kernel-probe/.probe.o.cmd
  mkdir "$out"
  cp defaults override "$out/"
  cp kernel-probe/.probe.o.cmd "$out/kernel-probe.cmd"
''
