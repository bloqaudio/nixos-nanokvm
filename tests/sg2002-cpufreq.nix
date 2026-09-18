{ pkgs, configurations }:
let
  inherit (pkgs) lib;
  check = pair:
    assert !pair.baseline.sg2002.cpuFreq.enable;
    assert pair.scaling.sg2002.cpuFreq.enable;
    assert lib.all (a: a.assertion) pair.scaling.assertions;
    assert pair.scaling.sg2002.auxCore.enable ->
      pair.scaling.sg2002.fdt.contractSha256 == pair.baseline.sg2002.fdt.contractSha256;
    ''
      python3 ${./verify-cpufreq-dtb.py} ${pair.baseline.sg2002.fdt} ${pair.scaling.sg2002.fdt}
      grep -qx CONFIG_CPU_FREQ=y ${pair.scaling.boot.kernelPackages.kernel.configfile}
      grep -qx CONFIG_CPUFREQ_DT=y ${pair.scaling.boot.kernelPackages.kernel.configfile}
      grep -qx CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y ${pair.scaling.boot.kernelPackages.kernel.configfile}
      grep -qx CONFIG_CPU_THERMAL=y ${pair.scaling.boot.kernelPackages.kernel.configfile}
    '';
in
pkgs.runCommand "sg2002-cpufreq-tests" {
  nativeBuildInputs = [ pkgs.python3 pkgs.dtc pkgs.gnugrep ];
} ''
  ${lib.concatMapStringsSep "\n" check configurations}
  touch "$out"
''
