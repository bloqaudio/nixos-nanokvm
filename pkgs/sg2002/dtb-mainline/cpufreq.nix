{ runCommand, dtc, gcc, linuxSrc, base }:
runCommand "sg2002-cpufreq.dtb" {
  nativeBuildInputs = [ dtc gcc ];
  # Preserve the carrier/C906L identity used by the composition checks.
  passthru = base.passthru or { };
} ''
  tar -xf ${linuxSrc} --wildcards '*/include/dt-bindings/clock/sophgo,cv1800.h' \
    '*/include/dt-bindings/thermal/thermal.h'
  src=$(echo linux-*/)
  test "$(fdtget -t s ${base} / compatible | tr ' ' '\n' | grep -x sophgo,sg2002)" = sophgo,sg2002
  # Absolute node references avoid depending on __symbols__ in the base DTB.
  dtc -I dtb -O dts ${base} > base.dts
  cat base.dts ${./sg2002-cpufreq.dtsi} > combined.dts
  cpp -nostdinc -undef -x assembler-with-cpp -I "$src/include" \
    combined.dts > combined.pre.dts
  dtc -I dts -O dtb combined.pre.dts -o "$out"
''
