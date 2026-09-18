{ pkgs, targetPkgs, kernel }:
let
  driver = targetPkgs.sg2002-aic8800-mainline-for kernel;
  filtered = driver.override { tcpAckFilter = true; };
  bluetoothDriver = targetPkgs.sg2002-aic8800-mainline-bluetooth-for
    (kernel.override { bluetooth = true; });
  bluetoothFiltered = bluetoothDriver.override { tcpAckFilter = true; };
  modulePath = "lib/modules/${kernel.modDirVersion}/kernel/drivers/net/wireless/aic8800/aic8800_fdrv.ko";
in
assert !driver.tcpAckFilter;
assert filtered.tcpAckFilter;
assert !bluetoothDriver.tcpAckFilter;
assert bluetoothFiltered.tcpAckFilter;
pkgs.runCommand "sg2002-wifi-ack-filter-tests" {
  nativeBuildInputs = [ pkgs.binutils pkgs.gnugrep ];
} ''
  # Inspect compiled code, not just the Nix or Makefile setting. The normal
  # driver must omit the filter, and the explicit comparison build retain it.
  mkdir "$out"
  check_pair() {
    name=$1
    nm --defined-only "$2/${modulePath}" > "$out/$name-default.symbols"
    nm --defined-only "$3/${modulePath}" > "$out/$name-filtered.symbols"
    for symbol in filter_send_tcp_ack tcp_ack_handle_new; do
      if grep -Eq "[[:space:]]$symbol$" "$out/$name-default.symbols"; then
        echo "Unexpected ACK filter symbol in $name default: $symbol" >&2
        exit 1
      fi
      grep -Eq "[[:space:]]$symbol$" "$out/$name-filtered.symbols"
    done
  }
  check_pair wifi ${driver} ${filtered}
  check_pair bluetooth ${bluetoothDriver} ${bluetoothFiltered}
  grep -Eq '[[:space:]]btsdio_init$' "$out/bluetooth-default.symbols"
  grep -Eq '[[:space:]]btsdio_init$' "$out/bluetooth-filtered.symbols"
''
