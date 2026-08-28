{
  lib,
  nanokvm-factory-runtime,
  runCommand,
}:

runCommand "sg2002-coda980-firmware"
  {
    # The driver asks for the literal .bin path. Do not let the generic
    # hardware.firmware compressor rename it to coda980.bin.zst.
    passthru.compressFirmware = false;
    meta = {
      description = "SG2002 Coda980 H.264 firmware";
      homepage = "https://github.com/sipeed/NanoKVM/releases/tag/v1.4.2";
      license = lib.licenses.unfreeRedistributableFirmware;
      platforms = lib.platforms.linux;
    };
  }
  ''
    install -Dm444 \
      ${nanokvm-factory-runtime}/share/fw_vcodec/coda980.bin \
      "$out/lib/firmware/sophgo/sg2002/coda980.bin"
  ''
