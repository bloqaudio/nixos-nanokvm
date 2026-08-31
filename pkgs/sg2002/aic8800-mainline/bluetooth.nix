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

    # The vendor btlpm driver starts its Bluetooth rfkill soft-blocked.  That
    # prevents hardware.bluetooth.powerOnBoot / BlueZ AutoEnable from powering
    # the already-registered SDIO controller on this image.  Keep this policy
    # in the BT-only wrapper: the production WiFi package remains byte-for-byte
    # the original source and behaviour.
    sed -i 's/rfkill_init_sw_state(bt_rfk, true);/rfkill_init_sw_state(bt_rfk, false);/' aic8800_btlpm/rfkill.c

    # btsdio emits these once for every TX work item/frame and RX frame at
    # pr_info level.  Make them compiled-out debug messages while retaining
    # the vendor's lifecycle and error logging.
    sed -i 's/^#define AICBT_DBG_FLAG[[:space:]]*1/#define AICBT_DBG_FLAG          0/' aic8800_fdrv/aic_btsdio.h
    sed -i 's/AICBT_INFO("%s", data->hdev->name);/AICBT_DBG("%s", data->hdev->name);/' aic8800_fdrv/btsdio.c
    sed -i 's/AICBT_INFO("%s,%s", data->hdev->name,__func__);/AICBT_DBG("%s,%s", data->hdev->name,__func__);/' aic8800_fdrv/btsdio.c
    sed -i 's/AICBT_INFO("skb type %d",type);/AICBT_DBG("skb type %d",type);/' aic8800_fdrv/btsdio.c
    # btsdio_send_frame uses hdev directly (rather than data->hdev), so keep
    # this focused on the hot send path and leave open/close/flush lifecycle
    # messages as INFO.
    sed -i '/static int btsdio_send_frame/,/switch (hci_skb_pkt_type(skb))/s/AICBT_INFO("%s,%s", hdev->name,__func__);/AICBT_DBG("%s,%s", hdev->name,__func__);/' aic8800_fdrv/btsdio.c

    grep -q '^CONFIG_SDIO_BT[[:space:]]*=[[:space:]]*y' aic8800_bsp/Makefile
    grep -q '^CONFIG_SDIO_BT[[:space:]]*=[[:space:]]*y' aic8800_fdrv/Makefile
    grep -q 'rfkill_init_sw_state(bt_rfk, false);' aic8800_btlpm/rfkill.c
    grep -q '^#define AICBT_DBG_FLAG[[:space:]]*0' aic8800_fdrv/aic_btsdio.h
    sed -n '/static int btsdio_send_frame/,/switch (hci_skb_pkt_type(skb))/p' aic8800_fdrv/btsdio.c | grep -q 'AICBT_DBG("%s,%s", hdev->name,__func__);'
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
