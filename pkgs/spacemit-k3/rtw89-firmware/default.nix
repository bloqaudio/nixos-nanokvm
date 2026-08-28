{
  lib,
  linux-firmware,
  runCommand,
}:
runCommand "spacemit-k3-rtw89-firmware" {
  passthru.compressFirmware = false;
  meta = {
    description = "Realtek RTL8852BE firmware for SpacemiT K3 Pico-ITX";
    license = lib.licenses.unfreeRedistributableFirmware;
    maintainers = [lib.maintainers.georgewhewell];
    platforms = ["riscv64-linux"];
  };
} ''
  mkdir -p $out/lib/firmware/rtw89
  cp -L ${linux-firmware}/lib/firmware/rtw89/rtw8852b_fw-*.bin $out/lib/firmware/rtw89/
''
