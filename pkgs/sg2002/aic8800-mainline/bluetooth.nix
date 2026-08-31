# BT-enabled wrapper around the normal AIC8800 mainline driver.
#
# Keep the ordinary derivation in default.nix untouched: WiFi is production
# critical and its derivation must remain identical when Bluetooth is off.
{
  stdenv,
  lib,
  buildPackages,
  kernel,
  firmware,
  src,
}:
let
  base = import ./default.nix {
    inherit stdenv lib buildPackages kernel firmware src;
  };
in
base.overrideAttrs (old: {
  postPatch = (old.postPatch or "") + ''
    # Both sides of the vendor SDIO transport must agree: BSP selects and
    # patches combo firmware; FDRV registers the HCI_SDIO controller.
    sed -i 's/^CONFIG_SDIO_BT[[:space:]]*=[[:space:]]*n/CONFIG_SDIO_BT = y/' aic8800_bsp/Makefile
    sed -i 's/^CONFIG_SDIO_BT[[:space:]]*=[[:space:]]*n/CONFIG_SDIO_BT = y/' aic8800_fdrv/Makefile
    # Linux 7.x removed hci_dev.dev_type/HCI_PRIMARY; hci_alloc_dev()
    # now creates the primary controller, so this assignment is redundant.
    sed -i '/hdev->dev_type[[:space:]]*=[[:space:]]*HCI_PRIMARY;/d' aic8800_fdrv/btsdio.c
    grep -q '^CONFIG_SDIO_BT[[:space:]]*=[[:space:]]*y' aic8800_bsp/Makefile
    grep -q '^CONFIG_SDIO_BT[[:space:]]*=[[:space:]]*y' aic8800_fdrv/Makefile
  '';
  preBuild = (old.preBuild or "") + ''
    # C8A1:0082 is D80 even though this board's compatibility firmware
    # directory is named aic8800DC.  The D80 H entry selects this HBT image.
    for blob in \
      fmacfwbt_8800d80_h_u02.bin \
      fw_adid_8800d80_u02.bin \
      fw_patch_8800d80_u02.bin \
      fw_patch_8800d80_u02_ext0.bin \
      fw_patch_table_8800d80_u02.bin; do
      test -e ${firmware}/lib/firmware/aic8800_sdio/aic8800DC/$blob || {
        echo "ERROR: required AIC8800D80 SDIO-BT firmware missing: $blob" >&2
        exit 1
      }
    done
  '';
})
