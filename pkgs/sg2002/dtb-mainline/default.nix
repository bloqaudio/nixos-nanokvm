# Mainline DTBs for the Sipeed LicheeRV Nano B-W variant.
#
# Starts from the upstream `sg2002-licheerv-nano-b.dts` in the mainline
# kernel tree (passed in as `linuxSrc`) and concatenates one or more
# overlay dtsi files — dtc merges nodes, so later properties replace
# earlier ones. Two flavours:
#
#   .dtb     — bw.dtsi only (default: WiFi/SDIO1 enabled).
#   .dtbOled — bw.dtsi + bw-oled.dtsi (SDIO1 disabled, IIC1 + SH1107
#              child on the freed-up SD1 pads). Pair with builds that
#              also turn sg2002.wifi.enable off.
#   .dtbs    — directory-shaped wrapper for NixOS's
#              `hardware.deviceTree.package` (covers the default DTB).
{ lib
, runCommand
, dtc
, gcc
, linuxSrc
,
}:
let
  c906lMemoryMap = import ../c906l-memory-map.nix;
  # Each overlay has to be interpolated into the script body individually
  # — `toString [path1 path2]` doesn't trigger Nix's path-to-store import,
  # it just stringifies the raw source paths, which are then missing from
  # the build's closure. `${p}` per element does the import.
  buildDtb = name: overlays:
    runCommand "${name}.dtb"
      {
        nativeBuildInputs = [ dtc gcc ];
      } ''
      tar -xf ${linuxSrc}
      SRC=$(echo linux-*/)
      DTS=$SRC/arch/riscv/boot/dts/sophgo/sg2002-licheerv-nano-b.dts

      cat "$DTS" ${lib.concatMapStringsSep " " (p: "${p}") overlays} > merged.dts

      cpp -nostdinc -undef -x assembler-with-cpp \
        -I "$SRC/include" \
        -I "$SRC/arch/riscv/boot/dts/sophgo" \
        -o merged.pre.dts merged.dts

      dtc -I dts -O dtb -o "$out" merged.pre.dts
    '';

  dtb = buildDtb "sg2002-licheerv-nano-bw" [
    ./sg2002-licheerv-nano-bw.dtsi
  ];

  dtbOled = buildDtb "sg2002-licheerv-nano-bw-oled" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-nano-bw-oled.dtsi
  ];

  dtbNoWifi = buildDtb "sg2002-licheerv-nano-bw-nowifi" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-nano-bw-nowifi.dtsi
  ];

  # Bring-up DT for the FSBL-started C906L.  Its FIP and this DTB are an
  # atomic pair: Linux must never allocate from the final 2 MiB while the
  # auxiliary core is executing there.
  dtbNoWifiC906L =
    let
      unchecked = buildDtb "sg2002-licheerv-nano-bw-nowifi-c906l-unchecked" [
        ./sg2002-licheerv-nano-bw.dtsi
        ./sg2002-licheerv-nano-bw-nowifi.dtsi
        ./sg2002-c906l.dtsi
      ];
    in
    runCommand "sg2002-licheerv-nano-bw-nowifi-c906l.dtb"
      {
        nativeBuildInputs = [ dtc ];
        passthru = c906lMemoryMap;
      } ''
      cp ${unchecked} "$out"

      # The DTS is intentionally readable and reviewable rather than Nix-
      # generated.  Verify its compiled contract against the shared memory-map
      # values so hand edits cannot drift from firmware/FIP/U-Boot packaging.
      test "$(fdtget -t x "$out" /reserved-memory/c906l-firmware@8fe00000 reg)" = \
        "${lib.toLower (lib.toHexString c906lMemoryMap.firmwareAddress)} ${lib.toLower (lib.toHexString c906lMemoryMap.firmwareSize)}"
      test "$(fdtget -t x "$out" /reserved-memory/c906l-shmem@8ff00000 reg)" = \
        "${lib.toLower (lib.toHexString c906lMemoryMap.sharedMemoryAddress)} ${lib.toLower (lib.toHexString c906lMemoryMap.sharedMemorySize)}"
      test "$(fdtget -t s "$out" /c906l-control compatible)" = \
        "sophgo,sg2002-c906l-control"
      test "$(fdtget -t s "$out" /c906l-rproc compatible)" = \
        "sophgo,sg2002-c906l-rproc"
      set -- $(fdtget -t x "$out" /c906l-rproc mboxes)
      test "$#" -eq 6
      test "$1" = "$4"
      test "$2 $3 $5 $6" = "1 2 2 2"
      test "$(fdtget -t s "$out" /c906l-rproc mbox-names)" = \
        "vq-kick vq-notify"
      test "$(fdtget -t s "$out" /soc/mailbox@1900000 compatible)" = \
        "sophgo,cv1800b-mailbox"
    '';

  # LicheeRV-Nano with the RJ45 wired: gmac0 + internal EPHY on.
  dtbEth = buildDtb "sg2002-licheerv-nano-bw-eth" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-eth.dtsi
  ];

  # Experimental USB handoff A/Bs.  The high-speed override is always
  # concatenated last, leaving the full-speed production DTBs untouched.
  dtbHighSpeed = buildDtb "sg2002-licheerv-nano-bw-high-speed" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-usb-high-speed.dtsi
  ];

  dtbNoWifiHighSpeed = buildDtb "sg2002-licheerv-nano-bw-nowifi-high-speed" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-nano-bw-nowifi.dtsi
    ./sg2002-usb-high-speed.dtsi
  ];

  # PicoClaw: keep the proven no-WiFi USB/NFS base, then add the onboard
  # ST7789 SPI panel and its three GPIO control lines.
  dtbPicoClawLcd = buildDtb "sg2002-licheerv-nano-picoclaw-lcd" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-nano-bw-nowifi.dtsi
    ./sg2002-licheerv-nano-picoclaw-lcd.dtsi
  ];

  # PicoClaw WiFi-root variant: retain the B-W board's SDIO1/AIC8800
  # wiring while adding the ST7789 panel. The LCD consumes SPI1/GPIOs,
  # not the SDIO1 pins, so the overlays can coexist.
  dtbPicoClawLcdWifi = buildDtb "sg2002-licheerv-nano-picoclaw-lcd-wifi" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-nano-picoclaw-lcd.dtsi
  ];

  dtbPicoClawLcdHighSpeed = buildDtb "sg2002-licheerv-nano-picoclaw-lcd-high-speed" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-nano-bw-nowifi.dtsi
    ./sg2002-licheerv-nano-picoclaw-lcd.dtsi
    ./sg2002-usb-high-speed.dtsi
  ];

  # NanoKVM-PCIe: bw.dtsi (WiFi/SDIO1 on) + ethernet enable overlay.
  dtbPcie = buildDtb "sg2002-nanokvm-pcie" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-nanokvm-pcie.dtsi
  ];

  # LicheeRV-Nano with the GC4653 camera FFC: ethernet + camera overlay
  # (IIC4 on PWR_WAKEUP0/PWR_BUTTON1, CAM_MCLK1 on MIPIRX0N, sensor reset
  # on GPIOE1, 2-lane CSI capture).
  dtbCam = buildDtb "sg2002-licheerv-nano-bw-cam" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-licheerv-nano-bw-nowifi.dtsi
    ./sg2002-licheerv-eth.dtsi
    ./sg2002-licheerv-camera-gc4653.dtsi
  ];

  dtbPcieNoWifi = buildDtb "sg2002-nanokvm-pcie-nowifi" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-nanokvm-pcie.dtsi
    ./sg2002-licheerv-nano-bw-nowifi.dtsi
  ];

  dtbPcieHighSpeed = buildDtb "sg2002-nanokvm-pcie-high-speed" [
    ./sg2002-licheerv-nano-bw.dtsi
    ./sg2002-nanokvm-pcie.dtsi
    ./sg2002-usb-high-speed.dtsi
  ];

  dtbs = runCommand "sg2002-dtbs" { } ''
    mkdir -p $out/sophgo
    cp ${dtb} $out/sophgo/sg2002-licheerv-nano-bw.dtb
  '';
in
{
  inherit dtb dtbs;
  high-speed = dtbHighSpeed;
  eth = dtbEth;
  oled = dtbOled;
  nowifi = dtbNoWifi;
  nowifi-c906l = dtbNoWifiC906L;
  nowifi-high-speed = dtbNoWifiHighSpeed;
  picoclaw-lcd = dtbPicoClawLcd;
  picoclaw-lcd-wifi = dtbPicoClawLcdWifi;
  picoclaw-lcd-high-speed = dtbPicoClawLcdHighSpeed;
  pcie = dtbPcie;
  pcie-nowifi = dtbPcieNoWifi;
  pcie-high-speed = dtbPcieHighSpeed;
  cam = dtbCam;
}
