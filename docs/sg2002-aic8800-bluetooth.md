# AIC8800 Bluetooth on SG2002 / PicoClaw

Bluetooth is opt-in: set `sg2002.bluetooth.enable = true` on a mainline
configuration that imports the AIC8800 WiFi support.  The catalog publishes
the intentionally separate PicoClaw test artifact:

```console
nix build .#legacyPackages.x86_64-linux.boards.picoclaw.mainline.live.wifi-bluetooth.payload
```

It does not use UART.  The AIC FDRV registers an `HCI_SDIO` controller, sends
HCI packets through the AIC firmware mailbox, and receives them through the
same message channel as WiFi.  WiFi and Bluetooth therefore share both the
SDIO link and combo firmware; leave this artifact opt-in until the target has
survived sustained mixed traffic.

## Firmware identity

The PicoClaw's observed SDIO IDs are `c8a1:0082` and `c8a1:0182`.  In the
pinned AIC source, `0x0082` maps to `PRODUCT_ID_AIC8800D80`, not the distinct
`AIC8800DC` product ID (`0xc08d`).  The Nix firmware directory remains named
`aic8800DC` only as a compatibility path used by the driver.  The observed
D80 U02/H load sequence is:

```text
fw_patch_table_8800d80_u02.bin
fw_adid_8800d80_u02.bin
fw_patch_8800d80_u02.bin (+ _ext0 when requested)
fmacfw_8800d80_h_u02.bin
```

With SDIO Bluetooth enabled, the D80 H entry selects
`fmacfwbt_8800d80_h_u02.bin`; the package checks that file and the D80 patch
tuple are present before compiling the BT driver.  WiFi-only continues to use
the original non-BT AIC driver derivation and firmware selection.

## First hardware validation

Do not test on an image carrying an OLED DTB: its SDIO1 pins are disabled.
After booting the dedicated PicoClaw artifact, collect the following before
pairing anything:

```console
rfkill list
lsmod | grep -E 'bluetooth|aic8800'
dmesg | grep -Ei 'aic|bluetooth|hci'
hciconfig -a
btmgmt info
bluetoothctl show
bluetoothctl --timeout 20 scan on
```

Expected evidence is an unblocked Bluetooth rfkill entry, `hci0` with
`Bus: SDIO`, and `bluetoothd` running.  The Bluetooth-only package changes the
vendor btlpm default from soft-blocked to unblocked so that BlueZ AutoEnable
can power the controller; `rfkill unblock bluetooth` remains the manual
recovery command if a user blocks it later.  It also compiles out the vendor's
per-HCI-frame `aic_btsdio` info spam, retaining error and lifecycle logging.
If there is no `hci0`, retain the `dmesg` output and do not retry by loading a
UART HCI driver: this board's implemented path is the AIC SDIO mailbox.

The BT-only kernel also provides the `bnep` module for BlueZ's Bluetooth PAN
profile.  This is unrelated to discovery, but avoids `bnep_init()` reporting
that the kernel lacks BNEP protocol support.  BlueZ 5.86 can additionally log
`Failed to set default system config for hci0` with a stock `main.conf`; its
upstream 5.87 fix identifies this as an empty-default-list startup warning,
not an AIC transport failure.  Treat it separately from controller errors and
use `btmgmt info` plus an actual scan to assess the radio.

## Reset recovery

The ordinary Linux reboot has previously stopped stage 2 without returning
the device to ROM.  On the live target, `sg2002-watchdog-keeper` owns the
nowayout DesignWare watchdog with an 85-second timeout.  The safe proposed
recovery test is to freeze that unit, rather than touching watchdog hardware
directly:

```console
systemctl freeze sg2002-watchdog-keeper.service
```

This intentionally stops petting while keeping the watchdog armed, so expect
the existing transport to disappear and a hardware reset within about 85
seconds.  It has not been triggered by this change; arrange a ROM/FIP observer
before using it.
